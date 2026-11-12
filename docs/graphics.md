# Graphics APIs and lighting

The editor and the 3D renderer can run on OpenGL 3.3, Vulkan, or Direct3D 12. Shaders are written once as Vulkan GLSL in `shaders/rhi` and cross-compiled at build time with glslang (SPIR-V), SPIRV-Cross (HLSL and GLSL 330) and DXC (DXIL).

## Choosing an API

- Editor Settings, Graphics: OpenGL, Vulkan, or Direct3D 12. Saved for the next launch.
- A 3D realm can set its own API in the Lighting panel. That is also used the next time the editor or `ts_game` starts.
- Command line, which wins over both: `--gfx=gl`, `--gfx=vk`, `--gfx=dx12`.

If Vulkan or Direct3D 12 cannot start, the editor opens on OpenGL and says why. Direct3D 12 is built on Windows only. Linux builds are OpenGL and Vulkan.

The classic OpenGL renderer is still what a scene uses when the API is OpenGL and the lighting preset is Classic, so existing scenes stay on the path they already have. Any other preset, or Vulkan / Direct3D 12, uses the shared forward renderer.

## What to try

`ts_admin --smoke-gfx` renders one lit cube on each backend that can start and compares it to the classic OpenGL frame. A backend that is not installed is skipped.

On a Windows machine with an RTX-class GPU:

1. `ts_admin --gfx=vk` and `--gfx=dx12`. The Forge, a 3D realm, and an undocked panel should look like OpenGL, including multi-viewport.
2. Lighting panel: Classic matches today's frame. Balanced and Cinematic turn on cascades, SSAO, bloom and a different tonemap.
3. Hardware ray tracing stays off, and the checkbox appears only when the GPU reports Vulkan ray queries or DirectX Raytracing tier 1.1.

## Lighting

Passes are modules (`src/render3d/lighting`). A plugin registers one with `register_lighting_module`. Built-ins:

- Shadows: 1–4 cascades, PCF or PCSS. Spot and area lights are light components; point lights remain.
- Global illumination: none, baked probes, a screen-space term, or hardware ray tracing.
- Ambient occlusion: off, SSAO, or ray traced.
- Reflections: off, probes, screen-space, or ray traced.
- Post: Reinhard (in the mesh shader, the classic look), Reinhard or ACES in a post pass, exposure, bloom, contrast and saturation.
- Parallax occlusion, driven by the normal map alpha when the scale is above zero.

Ray tracing is off unless the scene asks for it, and the controls that need it are hidden when the device cannot do it.
