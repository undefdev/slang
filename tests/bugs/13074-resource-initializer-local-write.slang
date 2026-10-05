// `selectThroughLocal` writes only its own local variables. No code outside the call can observe
// those stores, so moving the call to the start of each entry point preserves behavior. This test
// also covers `selectedTexture`: IR lowering packs that immutable source parameter into a
// synthesized parameter group, which must not make the initializer appear to read mutable global
// storage.
//
//TEST:SIMPLE: -target hlsl -entry computeMain -stage compute -O0 -validate-ir -o 13074-resource-initializer-local-write.hlsl
//TEST(compute):COMPARE_COMPUTE(filecheck-buffer=CHECK): -cpu -compute -shaderobj -output-using-type

// CHECK: type: float
// CHECK-NEXT: 0.000000
// CHECK-NEXT: 0.000000
// CHECK-NEXT: 0.000000
// CHECK-NEXT: 0.000000

//TEST_INPUT: Texture2D(size=4, content = one):name=firstTexture
Texture2D<float4> firstTexture;

//TEST_INPUT: Texture2D(size=4, content = zero):name=secondTexture
Texture2D<float4> secondTexture;

//TEST_INPUT: uniform(data=[1]):name=selectedTexture
uniform uint selectedTexture;

//TEST_INPUT: ubuffer(data=[0 0 0 0], stride=16):out,name=output
RWStructuredBuffer<float4> output;

[noinline]
Texture2D<float4> selectThroughLocal(
    Texture2D<float4> first,
    Texture2D<float4> second,
    uint index)
{
    Texture2D<float4> firstLocal = first;
    Texture2D<float4> secondLocal = second;
    Ptr<Texture2D<float4>> selectedLocal = index == 0 ? &firstLocal : &secondLocal;
    return *selectedLocal;
}

static Texture2D<float4> cachedTexture =
    selectThroughLocal(firstTexture, secondTexture, selectedTexture);

[numthreads(1, 1, 1)]
void computeMain()
{
    output[0] = cachedTexture.Load(int3(0, 0, 0));
}
