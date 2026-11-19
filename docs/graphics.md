# Graphics APIs and lighting

The editor and the 3D renderer can run on OpenGL 3.3, Vulkan, or Direct3D 12. Shaders are written once as Vulkan GLSL in `shaders/rhi` and cross-compiled at build time with glslang (SPIR-V), SPIRV-Cross (HLSL and GLSL 330) and DXC (DXIL).

## Choosing an API

Precedence is `--gfx=` , then the project, then the engine default.

- Editor Settings, Graphics: the engine default. A new install uses Vulkan when that device can be created, otherwise OpenGL. Direct3D 12 is a choice, not the default.
- Project settings (Realm menu in a 3D project, File menu in a 2D project): Use engine default, or OpenGL, Vulkan, or Direct3D 12. Saved in `project.json` as `graphics_api` so the team shares it. Opening a project restarts the editor on that API and says so. If the API cannot start, the editor stays on OpenGL and says why.
- `ts_game` uses the same project setting (and the engine default in `TombStoneConfig/settings.json` when the project does not name one).
- Command line, which wins over both: `--gfx=gl`, `--gfx=vk`, `--gfx=dx12`. `--open <project>` starts the editor straight in that project (the restart uses this).
- `--gfx-debug` turns on the Direct3D 12 debug layer and prints the `ID3D12InfoQueue` text when a call fails.
- `--gfx-warp` forces the Direct3D 12 WARP software adapter.

Direct3D 12 is built on Windows only. Linux builds are OpenGL and Vulkan.

The classic OpenGL renderer is still what a scene uses when the API is OpenGL and the lighting preset is Classic, so existing scenes stay on the path they already have. Any other preset, or Vulkan / Direct3D 12, uses the shared forward renderer.

## What to try

`ts_admin --smoke-gfx` renders one lit cube on each backend that can start and compares it to the classic OpenGL frame. It prints the backend, the adapter name and whether ray tracing is available. A backend that is not installed is skipped. Direct3D 12 is not skipped: Windows always has WARP, and a pipeline failure fails the run. `--gfx-warp` pins that run to WARP.

`ts_admin --smoke-3d` renders the same 3D scene the editor draws. With no `--gfx` it stays on the classic OpenGL renderer. `--gfx=vk` or `--gfx=dx12` renders that scene on the chosen backend and prints the adapter and ray-tracing support.

`ts_admin --smoke-present` draws a few frames into the real swapchain on Vulkan and Direct3D 12 and reads the backbuffer back. A black or uncleared image fails the run. `--gfx=` limits it to one API. `--gfx-warp` runs the Direct3D 12 half on WARP.

The device is the highest-performance discrete GPU (`DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`, then the largest dedicated video memory). Software, remote and virtual adapters are skipped, and the same preference is applied to the Vulkan physical device.

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

The shader bake runs `dxc` for the CPU that is configuring the build. The default is the fetched package's `bin/x64` (AMD64) or `bin/arm64`, which ships `dxc.exe`, `dxcompiler.dll` and `dxil.dll` together. `VULKAN_SDK/Bin/dxc.exe` is only used when that package is not there: SDK 1.4 Bin has no `dxil.dll`, which is normal for DXC 1.8 and newer. `-DNID_DXC_EXECUTABLE` overrides both. Configure runs `dxc --version` and stops if the binary is the wrong architecture. `dxil.dll` is copied next to `ts_admin.exe` only when the chosen compiler has it.
