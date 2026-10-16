// SceneUniforms shared by every scene shader. Sanctum's C++ app mirrors this
// struct by hand and asserts the byte size against goose reflection at
// pipeline load (see Scene::load in src/main.cpp); edit both together.
// Layout follows std140-ish uniform buffer rules as emitted by Slang for MSL.
public struct SceneUniforms
{
    public float3 iResolution;  // render target size in pixels
    public float  iTime;        // seconds since app start
    public float2 iMouse;       // -1..1, y up; stays at last position
    public float  iClick;       // 1.0 while left button is down
    public float  iAspect;      // width / height
}
