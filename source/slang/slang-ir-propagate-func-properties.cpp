#include "slang-ir-propagate-func-properties.h"

#include "slang-ir-insts.h"
#include "slang-ir-util.h"
#include "slang-ir.h"


namespace Slang
{
class FuncPropertyPropagationContext
{
public:
    virtual bool canProcess(IRFunc* f) = 0;
    virtual bool isInitialFunc(IRFunc* f) = 0;
    virtual bool propagate(IRBuilder& builder, IRFunc* func) = 0;
};

static bool isKnownOpCodeWithSideEffect(IROp op)
{
    switch (op)
    {
    case kIROp_IfElse:
    case kIROp_UnconditionalBranch:
    case kIROp_Switch:
    case kIROp_Return:
    case kIROp_Loop:
    case kIROp_Call:
    case kIROp_Param:
    case kIROp_Unreachable:
    case kIROp_Store:
    case kIROp_SwizzledStore:
        return true;
    default:
        return false;
    }
}

class ReadNoneFuncPropertyPropagationContext : public FuncPropertyPropagationContext
{
public:
    virtual bool isInitialFunc(IRFunc* f) override
    {
        // If the func has already been marked with any decorations, skip.
        for (auto decoration : f->getDecorations())
        {
            switch (decoration->getOp())
            {
            case kIROp_ReadNoneDecoration:
                return true;
            }
        }
        return false;
    }
    virtual bool canProcess(IRFunc* f) override
    {
        // If the func has already been marked with any decorations, skip.
        for (auto decoration : f->getDecorations())
        {
            switch (decoration->getOp())
            {
            case kIROp_ReadNoneDecoration:
            case kIROp_TargetIntrinsicDecoration:
                return false;
            }
        }
        return true;
    }

    virtual bool propagate(IRBuilder& builder, IRFunc* f) override
    {
        bool hasNonReadNoneOp = false;
        for (auto block : f->getBlocks())
        {
            for (auto inst : block->getChildren())
            {
                // Debug instructions describe the surrounding program but do not read its runtime
                // state. They therefore cannot affect whether the function is `ReadNone`.
                if (isDebugInfoInst(inst))
                    continue;

                // Generic side-effect metadata identifies most memory reads. Dedicated resource
                // operations are the exception: they can read resource contents without reporting
                // side effects. Either kind of read prevents `ReadNone`.
                if (!isKnownOpCodeWithSideEffect(inst->getOp()))
                {
                    if (inst->mightHaveSideEffects() || doesOpReadResourceContents(inst->getOp()))
                    {
                        hasNonReadNoneOp = true;
                        break;
                    }
                }

                if (auto call = as<IRCall>(inst))
                {
                    if (!isReadNoneCallee(call->getCallee()))
                    {
                        hasNonReadNoneOp = true;
                        break;
                    }
                }

                // Do any operands defined have pointer type of global or
                // unknown source? Passing them into a readNone callee may cause
                // side effects that breaks the readNone property.
                for (UInt o = 0; o < inst->getOperandCount(); o++)
                {
                    auto operand = inst->getOperand(o);
                    if (as<IRConstant>(operand))
                        continue;
                    if (as<IRType>(operand))
                        continue;
                    if (isGlobalOrUnknownMutableAddress(f, operand))
                    {
                        hasNonReadNoneOp = true;
                        break;
                    }
                }
            }
            if (hasNonReadNoneOp)
                break;
        }
        if (!hasNonReadNoneOp)
        {
            builder.addDecoration(f, kIROp_ReadNoneDecoration);
            return true;
        }
        return false;
    }
};

bool propagateFuncPropertiesImpl(IRModule* module, FuncPropertyPropagationContext* context)
{
    bool result = false;
    List<IRFunc*> workList;
    HashSet<IRFunc*> workListSet;

    auto addToWorkList = [&](IRFunc* f)
    {
        if (workListSet.add(f))
            workList.add(f);
    };
    auto addCallersToWorkList = [&](IRFunc* f)
    {
        if (auto g = findOuterGeneric(f))
        {
            for (auto use = g->firstUse; use; use = use->nextUse)
            {
                if (use->getUser()->getOp() == kIROp_Specialize)
                {
                    auto specialize = use->getUser();
                    for (auto iuse = specialize->firstUse; iuse; iuse = iuse->nextUse)
                    {
                        if (auto userFunc = getParentFunc(iuse->getUser()))
                            addToWorkList(userFunc);
                    }
                }
            }
            return;
        }
        for (auto use = f->firstUse; use; use = use->nextUse)
        {
            if (use->getUser()->getOp() == kIROp_Call)
            {
                if (auto userFunc = getParentFunc(use->getUser()))
                    addToWorkList(userFunc);
            }
        }
    };
    for (;;)
    {
        bool changed = false;
        workList.clear();
        workListSet.clear();

        // Add side effect free functions and their transitive callers to work list.
        for (auto inst : module->getGlobalInsts())
        {
            auto genericInst = as<IRGeneric>(inst);
            if (genericInst)
            {
                inst = findGenericReturnVal(genericInst);
            }
            if (auto func = as<IRFunc>(inst))
            {
                if (context->isInitialFunc(func))
                {
                    addCallersToWorkList(func);
                }
            }
        }

        // Add remaining functions to work list.
        for (auto inst : module->getGlobalInsts())
        {
            auto genericInst = as<IRGeneric>(inst);
            if (genericInst)
            {
                inst = findGenericReturnVal(genericInst);
            }
            if (auto func = as<IRFunc>(inst))
            {
                addToWorkList(func);
            }
        }

        IRBuilder builder(module);

        for (Index i = 0; i < workList.getCount(); i++)
        {
            auto f = workList[i];
            if (!context->canProcess(f))
                continue;

            // Never propagate to functions without a body.
            if (f->getFirstBlock() == nullptr)
                continue;

            if (context->propagate(builder, f))
            {
                addCallersToWorkList(f);
                changed = true;
            }
        }
        result |= changed;
        if (!changed)
            break;
    }
    return result;
}

class NoSideEffectFuncPropertyPropagationContext : public FuncPropertyPropagationContext
{
public:
    virtual bool canProcess(IRFunc* f) override
    {
        // If the func has already been marked with any decorations, skip.
        for (auto decoration : f->getDecorations())
        {
            switch (decoration->getOp())
            {
            case kIROp_ReadNoneDecoration:
            case kIROp_NoSideEffectDecoration:
            case kIROp_TargetIntrinsicDecoration:
                return false;
            }
        }
        return true;
    }
    virtual bool isInitialFunc(IRFunc* f) override
    {
        // If the func has already been marked with any decorations, skip.
        for (auto decoration : f->getDecorations())
        {
            switch (decoration->getOp())
            {
            case kIROp_ReadNoneDecoration:
            case kIROp_NoSideEffectDecoration:
                return true;
            }
        }
        return false;
    }
    virtual bool propagate(IRBuilder& builder, IRFunc* f) override
    {
        bool hasSideEffectCall = false;
        for (auto block : f->getBlocks())
        {
            for (auto inst : block->getChildren())
            {
                // Debug instructions do not change program state, so they cannot invalidate a
                // `NoSideEffect` result.
                if (isDebugInfoInst(inst))
                    continue;

                if (!isKnownOpCodeWithSideEffect(inst->getOp()))
                {
                    // For instructions outside the small set handled below, the generic
                    // side-effect classification gives us the complete answer.
                    if (inst->mightHaveSideEffects())
                    {
                        hasSideEffectCall = true;
                        break;
                    }
                    continue;
                }

                if (auto call = as<IRCall>(inst))
                {
                    if (!isNoSideEffectCallee(call->getCallee()))
                    {
                        hasSideEffectCall = true;
                        break;
                    }
                }

                // Do any operands defined have pointer type of global or
                // unknown source? Passing them into a NoSideEffect callee may cause
                // side effects that breaks the NoSideEffect property.
                for (UInt o = 0; o < inst->getOperandCount(); o++)
                {
                    auto operand = inst->getOperand(o);
                    if (as<IRConstant>(operand))
                        continue;
                    if (as<IRType>(operand))
                        continue;
                    if (isGlobalOrUnknownMutableAddress(f, operand))
                    {
                        hasSideEffectCall = true;
                        break;
                    }
                }
            }
            if (hasSideEffectCall)
                break;
        }
        if (!hasSideEffectCall)
        {
            builder.addDecoration(f, kIROp_NoSideEffectDecoration);
            return true;
        }
        return false;
    }
};

bool propagateFuncProperties(IRModule* module)
{
    ReadNoneFuncPropertyPropagationContext readNoneContext;
    bool changed = propagateFuncPropertiesImpl(module, &readNoneContext);

    NoSideEffectFuncPropertyPropagationContext noSideEffectContext;
    changed |= propagateFuncPropertiesImpl(module, &noSideEffectContext);

    return changed;
}

bool propagatePropertiesForSingleFunc(IRModule* module, IRFunc* f)
{
    ReadNoneFuncPropertyPropagationContext readNoneContext;
    bool changed = false;
    IRBuilder builder(module);
    if (readNoneContext.canProcess(f))
        changed |= readNoneContext.propagate(builder, f);

    NoSideEffectFuncPropertyPropagationContext noSideEffectContext;
    if (noSideEffectContext.canProcess(f))
        changed |= noSideEffectContext.propagate(builder, f);

    return changed;
}
} // namespace Slang
