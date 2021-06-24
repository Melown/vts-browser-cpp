
uniform sampler2D texColor;
uniform sampler2D texMask;

layout(std140) uniform uboSurface
{
    mat4 uniP;
    mat4 uniMv;
    vec4 uniUvTrans; // scale-x, scale-y, offset-x, offset-y
    vec4 uniColor;
    ivec4 uniFlags; // mask, monochromatic, flat shading, uv source, ..., frameIndex
};

in vec2 varUvTex;
in vec3 varViewPosition;
#ifdef VTS_ATM_PER_VERTEX
in float varAtmDensity;
#endif

layout(location = 0) out vec4 outColor;

bool getFlag(int i)
{
    return (uniFlags[i / 8] & (1 << (i % 8))) != 0;
}

void main()
{
    // derivatives computed before leaving uniform control flow
    vec2 uvDx = dFdx(varUvTex);
    vec2 uvDy = dFdy(varUvTex);
    vec3 viewDx = dFdx(varViewPosition);
    vec3 viewDy = dFdy(varViewPosition);

    // mask
    if (getFlag(0))
    {
        // assuming mipmaps are not needed:
        //   is textureLod more efficient than regular texture access?
        if (textureLod(texMask, varUvTex, 0.0).r < 0.5)
            discard;
    }

    if (getFlag(2))
    {
        // flat shading
        vec3 n = normalize(cross(viewDx, viewDy));
        outColor = vec4(vec3(max(n.z * 0.8, 0.0) + 0.125), 1.0);
    }
    else
    {
        // base color
        outColor = textureGrad(texColor, varUvTex, uvDx, uvDy);

        // monochromatic texture
        if (getFlag(1))
            outColor = outColor.rrra;
    }

    // atmosphere
    outColor *= uniColor;
#ifdef VTS_ATM_PER_VERTEX
    outColor = atmColor(varAtmDensity, outColor);
#else
    float atmDensity = atmDensity(varViewPosition);
    outColor = atmColor(atmDensity, outColor);
#endif
}

