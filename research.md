Research exactly how one would implement a direct directx9 32bit to arm + metal for macos translation layer for video games.

To help with this complex research, could you clarify a few points?

Are you interested in creating a full DX9-to-Metal translation layer from scratch, or leveraging existing projects like MoltenVK, Wine, or DXVK?

What is the target environment — are you aiming for compatibility with Apple Silicon (ARM64) Macs only, or both Intel and ARM Macs?

Is performance optimization a priority (e.g., for real-time gaming), or is functional compatibility more important?

Are you looking for architectural guidance, implementation code samples, or both?

This will help me tailor the research to your needs.

1. If moltenvk, wine, or dxvk could be used effectively to get high performance, i am open to using them
2. apple macs only 
3. performance optimization
4. I am looking for a deep architectural plan of implementation that goes beyond what is currently possible

Thanks for the details. I’ll investigate how one could architect and implement a high-performance DirectX 9 (32-bit) to ARM + Metal translation layer for macOS, potentially leveraging tools like Wine, DXVK, or MoltenVK, and aiming for a solution that exceeds current performance and compatibility limitations.

I'll let you know once I’ve gathered a deep implementation plan. Feel free to continue chatting with me while I work on it.

Research completed in 13m · 13 sources · 52 searches
High-Performance DirectX 9 to Metal Translation on Apple Silicon Macs
Introduction

Running legacy Windows DirectX 9 games on modern Apple Silicon Macs poses several challenges. These games are 32-bit x86 applications built for Windows and Direct3D 9, yet Apple Silicon uses an ARM64 CPU and supports only 64-bit macOS with the Metal graphics API. Apple’s macOS has completely dropped support for running 32-bit executables natively, so any 32-bit Windows game must be translated at both the CPU and graphics API levels. Despite these hurdles, a carefully designed translation layer can enable DirectX 9 games to run on Apple Silicon with high performance and compatibility. This report explores how to implement such a layer, focusing on translating the DirectX 9 fixed-function graphics pipeline to Metal, handling x86-to-ARM code execution, and leveraging existing projects (Wine, DXVK, MoltenVK) to achieve or surpass the current state-of-the-art.

DirectX 9 API to Metal API Translation

Mapping Fixed-Function Pipeline to Metal: DirectX 9 supports a fixed-function pipeline (FFP) for legacy graphics operations (transformations, lighting, texture blending) without explicit shaders. Modern APIs like Metal are fully shader-based and have no fixed-function pipeline. Thus, the translator must emulate fixed-function behaviors using Metal shaders and state. One approach (used in projects like DXVK’s D3D9 layer) is to dynamically generate shader code from the FFP state. For example, when a game sets FFP states for lighting (e.g. enabling a certain number of lights, material properties) or texture stages, the translation layer can create corresponding Metal Shading Language (MSL) vertex and fragment shaders that implement those states. This means computing vertex transforms (world/view/projection), lighting equations (e.g. Gouraud shading, fog), and texture blending in shader code that mimics what the D3D9 fixed pipeline would do.

 

Graphics Pipeline Equivalents: Many DirectX 9 render states and capabilities have direct or near-direct equivalents in Metal:

Vertex Formats: D3D9’s flexible vertex formats and declarations can be translated into Metal vertex descriptor layouts for vertex buffer binding.

Transformation Matrices: D3D9 SetTransform calls (world, view, projection matrices) can be passed into Metal shaders via uniform buffers. The translator may maintain a constant buffer for current transform matrices and update it on each change. The Metal vertex shader will multiply vertex positions by these matrices to emulate fixed-function transformation.

Lighting and Materials: If the game uses FFP lighting (via SetLight and material states), the translator can inject calculations in the Metal shader to compute vertex lighting (summing contributions of enabled lights with the specified material reflectance, much as the FFP would). For example, a basic Phong or Blinn-Phong model can be implemented in the shader to mimic D3D9’s vertex lighting.

Texture Blending and Sampler States: D3D9’s multi-stage texture blending (via SetTextureStageState) must be converted to Metal’s fragment shader logic. The translator can compile a fragment shader that samples the appropriate textures and applies operations (modulate, add, etc.) per the stage state configuration. Sampler states like filtering and address modes map to Metal’s MTLSamplerDescriptor. Features like shadow map compare (D3D9’s sampler state D3DSAMP_TEXTURETYPE=SHADOWMAP) correspond to Metal samplers with a compare function.

Alpha Test: DirectX 9 allowed an alpha test (rejecting pixels below a certain alpha threshold) via render states. Metal has no fixed alpha test stage, so the generated fragment shader should perform a discard_fragment (kill) based on the alpha reference to emulate this.

Point Sprites: D3D9 can automatically expand points into textured quads (point sprites) when enabled. Metal can handle this by using the built-in point size and point sprite functionality – in the vertex shader you set [[point_size]] and in the fragment shader use the [[point_coord]] to texture a point sprite, emulating D3D9’s point sprite semantics. If Metal lacks a feature, an alternative is to emulate in geometry shader or manually draw quads, but Metal on Apple GPUs does not support geometry shaders. Instead, one could use Metal’s sprite libraries or instanced quads to emulate point sprites if needed.

Shader Model Translation: DirectX 9’s programmable pipeline (Shader Model 2.0/3.0) uses HLSL which compiles to D3D9 bytecode (often called DX9 “shader assembly”). To run these on Metal, the translator must convert HLSL/DX9 shader bytecode into Metal’s shader language. A few strategies exist:

Via SPIR-V and MoltenVK: A practical path is to leverage DXVK, the Vulkan-based D3D9 implementation. DXVK’s D3D9 module will translate D3D9 drawing calls and shaders into Vulkan API calls and Vulkan SPIR-V shaders. If we route through Vulkan, we can then use MoltenVK to translate Vulkan to Metal. MoltenVK will accept the SPIR-V shaders and compile them to MSL under-the-hood. This chain ensures even custom HLSL shaders used in games are handled: DXVK uses HLSL->SPIR-V compilers (like d3dcompiler or DXVK’s built-in shader translator) to turn D3D9 shaders into SPIR-V, and MoltenVK/Metal then take over to produce native GPU code. The fixed-function aspects are also handled by DXVK which generates shaders from fixed-function state as needed, so that games using no custom shaders still get proper Metal shaders to execute.

Direct HLSL->MSL Conversion: For potentially higher performance and control, one could bypass Vulkan and compile D3D9 shaders directly to Metal. Apple’s Metal Shader Converter tool, for instance, is capable of converting DirectX shader IR (specifically DXIL for newer DirectX) to Metal IR. Although DXIL is the intermediate for DirectX 12, a pipeline could be envisioned where legacy HLSL or DX9 bytecode is first converted to a modern IR (e.g. by lifting Shader Model 3 bytecode to DXIL or SPIR-V) and then using Apple’s converter to Metal. The translator would need to account for differences in semantics (e.g. DirectX’s coordinate systems and conventions vs Metal’s). Notably, DirectX uses a coordinate system where the Y-axis is typically inverted relative to Metal’s, and a depth range of 0 to 1 (which Metal also uses, unlike OpenGL). Care must be taken to adjust pixel coordinates if needed (older D3D9 had a half-pixel offset rule for mapping texels to pixels, which should be handled in the shader or pipeline configuration to avoid one-pixel misalignment in rendering).

State and Draw Call Translation: Each Direct3D 9 draw call (e.g. DrawPrimitive, DrawIndexedPrimitive) will be translated to one or more Metal draw calls. The translation layer will maintain a command buffer for each frame and encode Metal draw commands corresponding to the D3D calls in order. D3D9’s concept of a “device” can be mapped to a Metal MTLDevice (the physical GPU) plus a MTLCommandQueue. For each frame (between BeginScene and EndScene in D3D9 terms), the translator can create a MTLCommandBuffer. When the game calls Present (swap buffers), the command buffer is committed to display the rendered frame on a CAMetalLayer.

 

One challenge is that DirectX 9 allowed mid-frame render target changes and StretchRect blit operations. These can be handled with Metal by ending the current render pass and beginning a new one with a different MTLRenderPassDescriptor (for render target changes), or using MTLBlitCommandEncoder for blits. Because Apple’s GPUs are tile-based deferred renderers (TBDR), switching render targets frequently can incur tile memory flushes, so the translator should batch operations when possible (e.g. do all draws to a render target, then switch, rather than flip-flopping repeatedly). Efficient use of Metal’s render passes (with MTLStoreAction set to .memoryless for temporary surfaces if possible) can hint to the GPU that intermediate results need not exit tile memory, improving performance.

x86–32bit to ARM64 Code Execution

Beyond graphics, the translation layer must execute the game’s 32-bit x86 CPU code on the ARM64 processor. Apple’s Rosetta 2 translation is highly optimized for x86_64 code, but notably does not support 32-bit x86 binaries at all. Therefore, we need an alternate solution for running 32-bit Windows game code on Apple Silicon. There are a few approaches:

Dynamic Binary Translation (Emulation): Incorporate a user-mode CPU emulator that translates x86 instructions to ARM64 on the fly. Projects like QEMU, FEX, or Box86/Box64 fall into this category. For example, the open-source Hangover project combines Wine with the FEX or Box64 emulators to run 32-bit and 64-bit Windows apps on ARM64 Linux. Hangover’s approach is to emulate only the application’s code, not an entire OS, and to break out of emulation whenever a Windows API call is made. In practice, this means the game’s x86 machine code is JIT-translated to ARM and executed until it needs to call an OS function (like Direct3D, file I/O, etc.), at which point control is handed to native code (the Wine libraries implemented in ARM64). By only emulating the app’s own logic and running system/translation code natively, this approach minimizes overhead. Modern emulators like FEX and Box64 have proven more efficient than older QEMU-based methods, reducing overhead and improving frame rates. Integrating a similar emulator into our Mac translation layer (or porting Hangover itself to macOS) would allow 32-bit games to run. The emulator would need to handle translating x86 instructions (including MMX/SSE used by game physics or 3D math) into ARM64, caching translated code for reuse to boost performance.

ARM64EC / Mixed-Mode Execution: Microsoft introduced the ARM64EC ABI for Windows on ARM, which allows mixing x86 and ARM code in one process (with x86 code calling into ARM64EC compiled libraries). While ARM64EC is Windows-specific, the concept could be applied: the translation layer’s own code (Wine, graphics translator, etc.) is compiled for ARM64, while the game’s x86 code runs under an emulator. Careful management is required for calling conventions and pointer sizes (32-bit app expects 32-bit pointers). Wine on ARM can operate in a WoW64-like mode where a 32-bit Windows program runs in a special environment on a 64-bit host. Our layer could set up a WoW64 container in Wine, where the 32-bit portions (the game’s code) are handled via emulation, and 64-bit portions (the core runtime) are native. This design allows switching between emulated and native execution on API boundaries, which is exactly how Hangover achieves good performance.

Virtualization / Containerization: Another route is to run the game inside a lightweight virtual machine that supports x86 execution. For instance, one could use a minimal x86 Windows or Linux container on Apple Silicon. Apple’s virtualization framework can run x86_64 Linux binaries under Rosetta inside an ARM VM, but not 32-bit. Alternatively, Windows 11 ARM edition can run 32-bit x86 games using Microsoft’s built-in emulator (as seen in Parallels Desktop solutions). However, a full VM has downsides: it introduces extra overhead (device virtualization, context switching) and complicates integration with the host (graphics calls would need to be forwarded to Metal, e.g. via a networked GPU protocol or GPU pass-through, which on Mac is not trivial). For maximum performance, an in-process emulator approach is preferred over full VMs – it avoids the cost of virtualizing hardware and allows direct use of host GPU APIs. That said, if an in-process solution proves too slow for certain games, a last-resort could be an ARM64 Linux container running Wine/DXVK (with Rosetta assisting for x86_64 and Hangover for x86) – essentially running the Linux version of this translation layer in a confined environment. This is complex and likely slower than a pure macOS solution, so we aim to avoid it unless necessary.

In summary, the translation layer should embed a fast x86→ARM JIT compiler. It will let the 32-bit game binary run, while intercepting Windows API calls and handing them off to our native implementations. By doing so, we achieve a seamless experience: to the game it appears as if it’s running on Windows with DirectX9, while under the hood its CPU instructions are being rewritten for Apple’s cores.

Leveraging Wine, DXVK, and MoltenVK

Building a high-performance translator from scratch would be an enormous effort, so we leverage existing open-source projects as building blocks:

Wine for Win32 API Translation: Wine provides reimplementations of Windows OS libraries in a portable way. By using Wine on macOS, we get a large portion of Windows functionality (file system calls, windowing, input handling, sound APIs, etc.) translated to their macOS equivalents. For example, when the game calls Win32 functions (CreateWindowEx, ReadFile, etc.), Wine’s libraries (compiled for ARM64) will handle them by calling into macOS system APIs or performing the needed tasks directly. Wine will also manage the creation of a window for the game, which on macOS can be backed by a NSWindow with a CAMetalLayer for drawing. This avoids needing a full Windows OS – instead, Wine acts as a compatibility layer, and our translation system runs within a Wine “prefix” environment.

DXVK for D3D9→Vulkan: DXVK is a proven high-performance translation layer that implements Direct3D 9 (as well as 10/11) on top of Vulkan. We can integrate DXVK’s D3D9 component as a replacement for Windows’ d3d9.dll in the Wine environment. When the game tries to create a D3D9 device or issues D3D9 draw calls, DXVK intercepts them and issues Vulkan commands to an abstracted Vulkan device. DXVK’s D3D9 implementation is very mature and handles most of DirectX 9, including tricky fixed-function cases and quirks of older games. In fact, DXVK explicitly tracks and emulates fixed-function pipeline state by generating shaders on the fly, which is crucial for compatibility with early-2000s games. By using DXVK, we inherit these capabilities and a large body of bug-fixes that target game-specific rendering issues. This can significantly shorten development time and improve compatibility, since many DirectX 9 games have idiosyncrasies that DXVK already accounts for (from wonky projection matrix handling to unsupported texture formats).

MoltenVK for Vulkan→Metal: MoltenVK is a Khronos-supported library that implements the Vulkan API on top of Apple’s Metal. By running DXVK over MoltenVK, all Vulkan calls from DXVK will be translated into equivalent Metal commands. MoltenVK handles mapping Vulkan constructs to Metal: command buffers, pipeline states, descriptor sets (mapped to Metal resource bindings or argument buffers), etc. It also compiles Vulkan SPIR-V shaders to Metal’s shader intermediate. In essence, MoltenVK allows us to reuse Vulkan-based translators on macOS. The combined chain D3D9 → DXVK (Vulkan) → MoltenVK (Metal) has been used in practice (for example, in CodeWeavers CrossOver and community projects) to run many DirectX 9 games on Mac. However, this chain does add some overhead (two translation layers back-to-back), which we will address below.

Integrating the Pieces: In our architecture, Wine loads the DXVK D3D9 library as d3d9.dll (via Wine’s DLL override mechanism). Meanwhile, MoltenVK is loaded as a Vulkan ICD (Installable Client Driver), so that when DXVK calls Vulkan functions, they are serviced by MoltenVK which in turn calls Metal framework functions. The data flow is thus:

Game calls D3D9 functions (e.g. IDirect3DDevice9::DrawPrimitive).

DXVK’s D3D9 layer (running as part of the Wine process) translates that into Vulkan commands (recorded into a Vulkan command buffer). It may also compile HLSL shader bytecode to SPIR-V using its shader compiler.

MoltenVK receives Vulkan calls (e.g. vkCmdDrawIndexed) and translates them to Metal calls via MTLCommandEncoder methods. When DXVK creates a Vulkan pipeline, MoltenVK compiles the SPIR-V to a MTLLibrary (Metal shader library) and sets up a corresponding MTLRenderPipelineState.

Metal executes on the GPU, rendering the frame.

Wine (with some help from us) takes care of presenting the resulting frame in an NSWindow, and handles input events or other OS interactions.

By leveraging these projects, we stand on the shoulders of existing solutions. Notably, Apple’s Game Porting Toolkit (GPTK) introduced in 2023 takes a similar hybrid approach – GPTK includes a D3DMetal translator which internally credits DXVK for some components. In fact, Apple’s GPTK now allows DirectX 11 and 12 games to run on Metal by translating DirectX API calls directly to Metal (bypassing Vulkan). However, as of this writing GPTK’s D3DMetal does not support DirectX 9’s fixed-function pipeline, so older titles still rely on the DXVK→MoltenVK path. Our solution can combine both: for newer D3D9 games that use only shaders (or even D3D11 titles), a direct-to-Metal path (inspired by Apple’s) could be used, whereas for older fixed-function-heavy games, the Vulkan/MoltenVK path ensures correctness. In the long run, we could extend Apple’s D3DMetal or write a similar layer for D3D9 so that DirectX 9 calls are translated straight to Metal without going through Vulkan. This would eliminate the overhead of the Vulkan abstraction and likely improve performance, as the translation can be tailored specifically to Metal’s API semantics and Apple GPU characteristics.

Overcoming Performance Limitations of Current Solutions

Current translation solutions (CrossOver/Wine, GPTK, etc.) work but face some performance and compatibility issues. We aim to overcome these:

CPU Emulation Overhead: Running x86 code on ARM incurs a performance tax. Early attempts at 32-bit support in CrossOver were extremely slow, since macOS lacks native 32-bit translation and an inefficient emulation had to be used. We address this by using state-of-the-art JIT emulators (like FEX or Box86) which significantly reduce overhead. These translate basic blocks of x86 code to ARM once and cache the translations, so code that runs many times (game logic loops) doesn’t incur repeated translation cost. Additionally, by breaking out of emulation for system calls, we leverage the full speed of native code for heavy tasks like rendering and I/O. A well-optimized emulator can even take advantage of vector extensions on Apple’s cores (mapping SSE instructions to ARM NEON where possible). The goal is to get CPU performance close to what the same game would achieve running under Windows-on-ARM’s own emulator (which is quite efficient – Windows 11 on ARM can run older PC games at surprisingly playable speeds). With careful optimization, the CPU overhead can be low enough that the GPU becomes the bottleneck (as is typical in games) rather than the translation.

Double Translation (Vulkan→Metal) Overhead: The DXVK→MoltenVK chain adds some layering cost and may not fully optimize for Apple’s GPU. For example, DXVK might issue Vulkan calls assuming a desktop discrete GPU with a certain memory model, and MoltenVK has to translate those to fit Metal’s expectations. This can lead to extra memory copies or suboptimal pipeline usage. To overcome this, we consider a more direct approach:

In the near term, we can tune MoltenVK for our use case. MoltenVK offers configuration options; for instance, it can use Apple’s argument buffers to implement Vulkan descriptor sets efficiently, and it can enable pipeline caching to avoid recompiling shaders each run. Ensuring MoltenVK is kept up-to-date and using Metal features (like heap objects, memoryless render targets, etc.) will cut overhead.

For the long term, implementing a native D3D9-to-Metal layer eliminates Vulkan overhead altogether. We would effectively do what DXVK does, but call Metal API directly. This is more work, but could yield performance closer to native. Apple’s D3DMetal for DX12/11 has shown that skipping the extra abstraction can improve performance for modern games. We would extend that concept to DX9 by writing a Metal backend for D3D9. We can still reuse logic from DXVK (e.g. its fixed-function shader generation algorithms) but instead of outputting Vulkan commands, we call Metal. This can reduce draw-call overhead and latency because we can match DirectX 9 concepts directly to Metal constructs one-to-one where possible, and avoid any translation mismatch.

Graphics API Feature Gaps: Certain DirectX 9 features don’t have direct equivalents in Metal or Vulkan, which can affect performance or accuracy. For example, D3D9’s half-precision shader hints (the _pp partial precision modifier in HLSL) or fixed 24-bit depth buffer precision might be emulated with full 32-bit floats in shaders and 32-bit depth in Metal. This is safer for correctness but could be slower on hardware that is capable of faster 16-bit float math. Apple’s GPUs have high performance 16-bit float (FP16) support. To optimize, the translator could detect when a D3D9 shader doesn’t require full 32-bit precision and use Metal’s half type in the MSL shader or enable fast math options. This must be balanced against accuracy – we would verify that using lower precision doesn’t visibly alter the game’s rendering (most likely it won’t, since many D3D9 era GPUs were themselves limited in precision). Another gap: D3D9’s lack of a concept like unified memory means some games expect slow LockRect operations on resources. On our translation layer, we can often do better by leveraging Apple’s unified memory – for instance, if a game locks a vertex buffer to update it each frame, we can give it a pointer to a mapped MTLBuffer and simply flush that region, avoiding an extra copy.

Synchronization Overhead: Wine historically had overhead translating Windows synchronization objects (events, mutexes) on macOS. Each WaitForObject or signal might involve heavy emulation if done the Linux way. Recently, MSync was introduced (in CrossOver 23) to map Wine sync primitives to native Mach primitives. We will adopt MSync’s approach: utilize Mach semaphores or Grand Central Dispatch synchronization for implementing Win32 critical sections, events, etc. This drastically lowers CPU usage in multi-threaded games. For example, many games have a “render thread” signalling an “update thread”; with MSync, these signals go through lightweight OS synchronization instead of busy-wait loops or expensive kernel-workarounds. Lower sync overhead means more CPU headroom for the game itself.

Latency and Frames per Second (FPS): Our translation layer aims for minimal added latency. One potential source of latency is if the translation buffers an extra frame (for example, if the emulator runs slightly decoupled from GPU submission). We will ensure that when the game calls Present, we immediately submit the Metal command buffer to display. Metal can present with vsync by default; we might give an option to disable vsync (allowing tearing) if a user prefers lower input latency. Additionally, by doing shader compilation upfront (either at game launch or scene load), we avoid runtime stutters. DXVK already implements an asynchronous pipeline compiler and a pipeline cache to reduce hitches when a new shader is encountered. We can use these features – for instance, compile all needed Metal pipeline states during loading screens (many games call CreateVertexShader/CreatePixelShader at load time – we intercept those and compile to Metal then, not during rendering). This way, gameplay remains smooth.

Compatibility Issues: Some older games rely on obscure behaviors (e.g. undefined DirectX behavior that the original drivers tolerated). The community-driven nature of Wine/DXVK helps here; many such issues have been identified and fixed. By staying close to those projects (and even contributing back any Mac-specific fixes), we inherit a lot of compatibility. For example, the fixed-function vertex fog in D3D9 might originally have a half-pixel offset bug – DXVK might have a workaround implemented to replicate how it looked on Windows GPUs, ensuring the game’s appearance matches the original. We will include these fixes. Another typical limitation is anti-cheat software in games, which often fails under Wine due to lack of Windows kernel drivers. Our user-mode translation cannot easily support kernel-mode anti-cheats (EasyAntiCheat, BattlEye). That is one area where no translation layer (on Mac or Linux) currently succeeds – the only solution there is a full Windows VM. We will note that such titles remain unsupported in the translation layer by design, focusing instead on single-player and less-protected games.

In summary, by using faster CPU translation, reducing redundant layers, optimizing shader precision, improving sync primitives, and leveraging known fixes, our solution can overcome the major bottlenecks that plague earlier attempts. For instance, CrossOver’s team noted that some 32-bit games ran an order of magnitude faster in a Windows 11 ARM VM than under their older Wine emulation; with our improvements (similar to what Hangover/FEX did on Linux and what Apple did with GPTK), we intend to close that gap, achieving comparable performance without a full VM.

Architecture of the Runtime Translation Layer

Our translation layer can be visualized as a stack of components, each handling a specific aspect of the problem. The architecture is modular, so each part can be optimized or replaced independently:

Windows Emulation & System Libraries (Wine): At the top, we have the Wine layer providing Win32 APIs. The game’s calls to user32, kernel32, directsound, etc. are handled here. Wine on macOS will use the Apple APIs under the hood (e.g., creating an NSWindow for the game, mapping Windows file access to the Mac file system, and using Core Audio for sound output). This forms the compatibility environment in which the game thinks it’s running on Windows.

CPU Instruction Translator: Sitting alongside Wine’s modules is the x86→ARM translator. This is essentially a JIT compiler that operates whenever the 32-bit game’s machine code needs to execute. We can implement this as a dynamic library that provides the Windows process environment (Wow64), or even as modifications to Wine’s loader to use an emulator core for 32-bit EXEs. This component ensures that any pure computation or game logic written in x86 is executed on the Apple Silicon cores. It interacts with Wine such that when the game invokes a Windows API (which lives in an ARM64 Wine DLL), control is transferred out of the emulator into native code. Conversely, when a callback or function pointer needs to go back into game code, the emulator resumes translation. This boundary is critical and is handled by thunking layers provided by Wine’s WoW64 support (for instance, using a call gate that switches the CPU mode).

DirectX Graphics Translator: For Direct3D 9, we plug in a D3D9 translation module. In the current design, this is DXVK’s d3d9.dll running under Wine. It accepts Direct3D 8/9 API calls and interfaces (the game’s IDirect3DDevice9, IDirect3DVertexBuffer9, etc.) and implements them using lower-level graphics calls. If using the Vulkan route, this module produces Vulkan API calls and shader code. In a more direct Metal route, this module could call Metal’s C++ API (via Objective-C++ bridging) to create MTLDevice resources and issue draw calls. Either way, this module is responsible for converting all graphics-related calls: creating textures, vertex buffers, handling locks and updates to those resources, setting shader constants, and issuing draw commands. It maintains necessary state tracking (e.g., current shaders or fixed-func state, current textures bound to each stage, current render target, etc.), because D3D9 expects the driver to remember state between calls. The translator uses this to decide when to create a new Metal pipeline state (for instance, if the game changes the alpha blending render state, the translator knows it may need a different Metal render pipeline configuration). Performance-sensitive parts, like draw calls, should be as lightweight as possible – ideally just translating into one or two Metal commands.

Vulkan-to-Metal (MoltenVK) Layer: If using Vulkan as an intermediate, MoltenVK acts as a sub-component under the D3D9 translator. The DXVK module will load the Vulkan library; on macOS this is provided by MoltenVK which implements all vk* functions. MoltenVK internally will call Metal framework functions. This layer is largely transparent in the architecture (it doesn’t require explicit management by our code beyond initialization), but we consider it part of the graphics translation pipeline. In a direct-to-Metal implementation, this layer is absent.

Audio and Input Handling: While graphics is the focus, a robust runtime must handle sound, input, and other I/O. DirectSound (the audio API in DirectX 9) can be implemented via Wine’s winecoreaudio driver, which uses CoreAudio on Mac to play sound. We ensure support for secondary sound buffers, 3D sound effects, etc., likely by mapping them to CoreAudio’s mixer or to OpenAL-soft. Input for games typically comes through Windows messages (keyboard, mouse via Win32 events) or DirectInput/XInput for game controllers. Wine will capture Mac input events (from Cocoa or IOKit) and translate them to Win32 messages. For mouse, we need to support relative mode (games often hide the cursor and track deltas). This can be done by using [NSWindow setMouseCursorVisibility:NO] and warping the cursor, or using CGAssociateMouseAndMouseCursorPosition(false) to decouple the mouse. Then intercepting mouse move events gives relative movement which Wine turns into DirectInput data. For gamepads, XInput is commonly used by games for Xbox 360/One controller support. We can implement the XInput DLL such that it interfaces with macOS’s Game Controller framework (GCController). For example, when the game calls XInputGetState(), our implementation will query the connected controller’s current state via GCController APIs and fill the XInput data structure accordingly. This ensures controllers are recognized and functioning. Older DirectInput for joysticks can also be mapped through IOKit HID interfaces on Mac.

Windowing and Fullscreen: The translation layer must also manage the window or fullscreen mode the game requests. D3D9 games often use IDirect3D9::CreateDevice with parameters indicating windowed or fullscreen, and can toggle fullscreen via Reset. Our layer will use these calls to drive macOS windowing: if fullscreen is requested, we can use macOS’s fullscreen Spaces (by calling [window toggleFullScreen]) or change display resolution. On macOS 13+, switching display resolution is possible via CGDisplayMode APIs – we can find the closest match for the requested resolution and switch the display if exclusive fullscreen is desired. Many modern wrappers prefer borderless windowed fullscreen (running a window at desktop resolution with no borders) to simplify things. We might do that: when a game requests 1024x768 fullscreen, we could actually keep the display at native (say 2560x1600) but create a borderless window and use Metal to scale up the game’s rendered output to fill the screen. However, this can introduce blurriness if not integer-scaled. Alternatively, actually switching the display mode to 1024x768 (if supported) would let the game render at that resolution natively. We must also deal with High-DPI (Retina) scaling – macOS might by default scale a low-res window. We likely will disable Retina scaling for the game window (setting NSWindow.backingScaleFactor or using a low-level property so that a “1 point = 1 pixel” in the game). In fact, testers found that turning off high-dpi mode improves performance for games by not pushing them to render at 4K resolution inadvertently.

Putting it all together, the architecture ensures that each subsystem (CPU, graphics, input, sound, etc.) is handled by an appropriate translator which interconnects with the others. The game process essentially thinks it’s running on Windows: it loads d3d9.dll and other DLLs, which are provided by our system (Wine/DXVK), and it executes its x86 code which is being transparently JIT-ed. Graphics commands flow through Wine → DXVK → MoltenVK → Metal, as described, unless we implement a direct path. The overall design is depicted below:

[ Windows 32-bit Game EXE (x86) ]
         ↳ *CPU instructions* – translated by → [ x86→ARM JIT Emulation ] – runs on Apple CPU  
         ↳ *Win32 API calls* – handled by → [ Wine libraries (ARM64) ] – calls macOS APIs  
         ↳ *Direct3D9 calls* – handled by → [ D3D9 Translation (DXVK/Metal) ] – issues Metal GPU commands  
         ↳ *DirectSound calls* – handled by → [ Wine CoreAudio driver ] – outputs via CoreAudio  
         ↳ *Input (Win32/DInput/XInput)* – handled by → [ Wine input + Mac frameworks ] – reads KB/M/Controller  
 
Metal commands from graphics layer → [ Apple Metal Driver ] → [ Apple GPU ] for rendering.  

(The arrows indicate how different calls from the game are routed to different components.)

 

This architecture allows each layer to be optimized independently. For instance, if Apple releases a new GPU feature, we could modify the Metal translation part to use it without altering the rest. Or if a better x86 emulator comes out, we can swap out our CPU translator. Thanks to Wine and DXVK, much of the heavy lifting in compatibility is already done, letting us focus on performance tuning.

Metal and Apple Silicon Specific Optimizations

To achieve high performance, we tailor the translation layer to Apple’s hardware strengths:

Tile-Based Deferred Rendering (TBDR): Apple’s GPUs (in Apple Silicon SoCs) use TBDR, meaning they divide the screen into tiles and render each tile completely in on-chip memory before writing out to slower RAM. To maximize performance on TBDR, we minimize state changes that break tile locality:

We aim to keep rendering within a single Metal render pass as long as possible. If a game issues multiple draw calls to the same render target consecutively (which is typical), we will encode them in one MTLRenderCommandEncoder. If the game switches render targets (e.g., rendering a shadow map then the frame), that forces ending one pass and starting another. We ensure to use MTLRenderPassDescriptor configurations that mark depth or color attachments as MTLStoreAction.memoryless when the data doesn’t need to be preserved, enabling the GPU to discard tile buffers instead of writing them out. This is beneficial for temporary render targets (like shadow maps or post-processing intermediates).

We also use Metal’s tile shading feature only if needed. (Metal allows a special tile shader that runs per tile for certain computations like deferred shading blending. DirectX 9 doesn’t directly need this, but if we implement MSAA resolve or complex post-processing, a tile shader could do custom resolves efficiently on-chip.)

Efficient Memory Use: With unified memory on Apple Silicon, CPU and GPU share the same physical memory. This means we can avoid extra copies between “system” and “video” memory that older PC architectures required. For dynamic resources:

If the game frequently updates a vertex buffer or texture each frame (using D3DLOCK_DISCARD or D3DLOCK_NOOVERWRITE flags), we allocate those buffers in shared memory and use simple memory mapping. For example, when a game calls LockRect on a texture to update it, we can provide a pointer into a MTLBuffer that backs the texture, so the game writes directly into it. After unlocking, we use MTLBlitCommandEncoder or texture replace to update the GPU resource in-place. Because it’s unified memory, often this is effectively zero-copy – the GPU can read the updated data directly. We will take care to triple-buffer such resources if the game expects to render from the previous frame’s data while updating a new frame’s data (avoiding GPU/CPU synchronization stalls).

We leverage MTLHeaps to manage memory allocations for resources, which can reduce fragmentation and allow better reuse of memory on the GPU. By allocating textures and buffers out of a Metal heap, we also hint to the driver to keep them in the optimal memory region. For rarely-changing resources, we might use private memory (GPU-only) for efficiency, but for dynamic ones, shared memory is ideal on Apple GPUs.

Shader Precompilation and Caching: As mentioned, we compile all required shaders to Metal’s binary format (.metallib) either ahead-of-time or at runtime startup. We utilize DXVK’s pipeline cache concept – on first run, the game might incur shader compile time, but we save the compiled Metal shader binaries to disk (similar to how DXVK caches SPIR-V). On subsequent runs, we can load these precompiled MTLLibrary binaries directly, bypassing compilation. This eliminates in-game stutters due to shader compilation. It also means that if many games share common shaders (some engines do), the cache can be reused. Apple’s Metal shader converter can assist by converting even complex shaders (like DX9 assembly or DXIL) offline on a development machine, and distributing metallib binaries with the translation layer.

Pipeline State Management: In DirectX 9, changing certain render states might require changing the graphics pipeline (shader + blend + rasterizer state). We will create Metal Pipeline State Objects (PSOs) on-the-fly as needed, but cache them aggressively. Each unique combination of vertex shader, fragment shader, blend state, rasterizer state, and depth state becomes a key in a PSO cache. Since D3D9 doesn’t have as many states as newer APIs, this is manageable. We can hash the state and shaders to reuse PSOs. Also, using Metal’s Pipeline State Programming Guide best practices: e.g., if many pipeline objects share the same shader code with small variations in states, we could use Metal’s function specializaion or MTLComputePipelineState (though mostly for compute, not needed for D3D9) to reduce compilation overhead.

Multi-threading and Command Submission: While D3D9 is largely single-threaded from the game’s perspective (unless the game explicitly uses the D3D9 multithread flag, which is rare), our layer can still parallelize internally. For instance, shader compilation can be done on a background thread while the game continues (DXVK already does this for Vulkan pipelines). We will ensure that our Metal compilation calls occur off the main thread as much as possible. Additionally, we might employ a separate thread to feed commands to the GPU: one thread records Metal commands from the game calls, another thread commits command buffers to the GPU queue. Apple’s Metal driver can usually handle this asynchronously, and using a dedicated thread for MTLCommandBuffer.commit() can overlap CPU and GPU work better. Care must be taken to preserve D3D9’s expected ordering (we can’t let a command buffer from one frame begin before the previous has finished if the game expects strict sequencing, unless the game uses Present with a non-blocking flag).

Metal-Specific Features: Apple continuously improves Metal; for example, Metal 3 introduced GPU-driven pipelines and mesh shaders (analogous to DX12 mesh shaders). While not needed for DX9 per se, if we ever want to remaster how a game renders (outside the original scope), we could use mesh shaders to simulate fixed-function T&L more efficiently. That is beyond a pure translation, though. More practically, we use Metal features like Fence and Event to synchronize GPU and CPU when needed (e.g., when the game locks a resource that the GPU is using, we insert a Metal fence to know when the GPU is done). This is similar to how DXVK would use Vulkan fences for LockRect on a render target. We make sure to use Metal’s lightweight MTLSharedEvent for cross-queue sync or dispatch_semaphore to stall CPU only when absolutely required.

Precision and Performance Hints: As noted earlier, we will use 16-bit floats (half type in MSL) in shaders when D3D9 precision rules allow it, because Apple GPUs can execute half-precision arithmetic much faster and in parallel. We also enable Metal’s fast math (allowing fused ops, etc.) since games usually don’t need IEEE strict compliance for shader math – this mimics D3D’s default of fast math optimizations. If any visual issues appear (rare), we could fall back to full precision for those shaders via a flag.

In combination, these optimizations exploit the Apple Silicon’s unified memory and tile rendering architecture to ensure the translation layer itself does not become the bottleneck. The end result should be that the game runs GPU-bound similar to a native game. For example, if a D3D9 game rendered at 100 FPS on a certain Apple GPU under ideal native conditions, our goal is to approach that frame rate by minimizing overhead. Early testing on similar systems (like Linux+Proton on x86) shows that translation layers can indeed reach performance parity or even exceed native Windows in some cases (due to driver differences). On macOS, with careful tuning, we expect a well-optimized translation layer to play older games at full speed, and even handle newer DX9 titles with heavy shaders gracefully.

System Integration Considerations

Making the translation layer feel seamless on macOS involves handling a few system-level integration points:

Fullscreen and Resolution Switching: Windows games often use exclusive fullscreen for better performance. On macOS, exclusive fullscreen is not exactly the same – but we can emulate it. When a game requests fullscreen at a given resolution, we have two options:

Exclusive Mode: Change the display resolution and make the Metal layer cover the display. We use CGDisplaySetDisplayMode to switch the active display to the closest supported mode (e.g. 1920×1080). We then resize our CAMetalLayer to that size and make the window borderless on that display. We must also hide the macOS menu bar and dock (macOS does this automatically when an app enters fullscreen space, or if we use the older CGCaptureAllDisplays API). The benefit is the game truly runs at the desired resolution without scaling. The drawback is on multi-monitor setups or if the resolution is non-native, it can mess with desktop icon positions or user experience when alt-tabbing.

Borderless Fullscreen Window: Many modern Mac games use this – we keep the desktop resolution, but create a fullscreen borderless window. We then have the Metal layer either render at the desktop size or render at the game’s intended resolution and upscale it. Upscaling can be done by Metal (drawing the final frame texture to the screen with bilinear filtering) or by letting the macOS WindowServer scale the layer. The deep integration would likely prefer doing it ourselves for control. The blog excerpt earlier showed that running games at very high retina resolution can tank performance, so often it's beneficial to render at a lower res and upscale. We can even expose an option to use Metal’s MTLDrawable.displaySyncEnabled = NO and present at unlocked frame rates in a borderless window for low-latency (but then we’d do our own frame throttle to avoid running unnecessarily fast).

We will also support alt-tab behavior: In Windows fullscreen, alt-tab minimizes the game. Under Wine/macOS, we can simulate this by switching out of fullscreen or pausing rendering when the app loses focus. We listen for NSWindow focus change or NSApplication deactivate notifications to know when to stop the game’s rendering (and possibly free GPU resources if needed). Upon refocus, we restore state and if needed reset the D3D9 device (some games require a device reset after alt-tab). Our layer will implement IDirect3DDevice9::TestCooperativeLevel and Reset so that games can handle device loss correctly – internally, though, a “device loss” might correspond to the user minimizing the window or a system sleep, etc., rather than an actual GPU loss.

Input Devices and Controllers: For keyboard and mouse, integration is straightforward via Wine. We will ensure our Wine build uses the macOS Quartz driver so that it uses native Mac window events. This lets us capture keyboard input including special keys. For mouse, as discussed, we handle relative mode by hiding the cursor and warping it to center or using CGEventSource to reset position, allowing unlimited movement. Wine translates these into Win32 messages or DirectInput data that the game receives. For gamepads, we incorporate an XInput->Mac adapter. Likely we can use an existing open-source implementation or write a thin one: when the game calls XInputGetState, we map to GCController.buttonA/B/X/Y etc. This requires a polling loop or event callback. We might run a small thread to update the controller state at 60 Hz and feed it to any waiting XInput calls. DirectInput (older joystick API) can be implemented via Wine’s dinput DLL, which on Mac could enumerate USB gamepads via IOKit. However, many games nowadays use XInput if they support controllers at all, so that will cover most cases. We will also ensure the translation layer supports force feedback if the game uses it (XInput rumble or DirectInput effects). On Mac, force feedback can be done with IOKit for supported devices. This adds to the completeness of the experience.

Sound and Music: DirectX 9 era games often use DirectSound or the older waveOut APIs. Wine implements these using CoreAudio or OpenAL. We will use Wine’s winecoreaudio which creates a CoreAudio output stream. It mixes multiple DirectSound “secondary buffers” into one output if needed, and supports 3D audio via calculations if the game uses it. For better performance and compatibility, we can set the CoreAudio stream to match the game’s requested sample rate and format (to avoid resampling overhead). Some games also use MIDI music (through DirectMusic or winmm). On macOS, we can route MIDI to the built-in DLS synthesizer or use a soundfont with Fluidsynth. This may be niche, but it’s part of making old games fully functional (e.g., some pre-2005 games have MIDI soundtracks). We ensure our Wine prefix includes a working MIDI synth so those games have music.

32-bit Address Space & Memory Constraints: One limitation of running a 32-bit game on a 64-bit system is the address space. A 32-bit game assumes it can use up to ~2GB or 4GB of address space. On our system, the process is actually 64-bit (since it hosts ARM64 code), but the game’s memory allocations should ideally stay within a 32-bit range for compatibility (some games have pointers stored in 32-bit variables). Wine’s WoW64 helps enforce that by allocating the game’s heap in the lower 4GB. We will need to be mindful not to allocate large buffers above that range if the game will see the pointer (for example, when the game calls Direct3D9 to create a vertex buffer, the returned IDirect3DVertexBuffer9* is a pointer to a COM object – that pointer can be 64-bit in a 64-bit process, but the game might only store it in a 32-bit space, which would corrupt it. Typically, Wine’s memory layout and the design of COM pointers avoids this by ensuring the COM interface pointers also fit the expected size). Testing and careful memory management will be needed to avoid issues with pointers/truncation in the emulated environment.

Use of a VM/Container: As a note, if some users prefer isolation or if we deliver this as a product, we might encapsulate the whole thing in a self-contained app or container (like CrossOver bottles or Apple’s game porting toolkit environment). This isn’t a full VM, but rather a packaging of the Wine prefix and emulator. A last resort would be actually using a lightweight VM running Linux+Hangover, but as analyzed, that introduces complexity. Instead, we focus on a native macOS application bundle that includes our Wine libraries, the emulator, MoltenVK, etc. The user just launches the game through this bundle. This approach uses Apple’s own process and memory, not a VM, thus staying lightweight.

By covering these integration points, we ensure that from the end-user perspective, the game behaves normally: you can switch in/out, use your game controller, hear all audio, and the game can even change settings like resolution or input options and have them work on macOS. The translation layer essentially acts as a compatibility runtime for the game on Mac, analogous to what Proton does on Linux.

Conclusion

Implementing a high-performance DirectX 9 translation layer for Apple Silicon Macs is a complex but achievable task. By translating the D3D9 fixed-function and shader pipeline to Metal, handling x86→ARM code through efficient JIT emulation, and leveraging battle-tested projects (Wine for Windows APIs, DXVK for D3D9, MoltenVK for Metal integration), we can run 32-bit Windows games on modern Macs with excellent speed and compatibility. We addressed key challenges such as the lack of native 32-bit support by employing advanced emulators, and matched legacy graphics features to Apple’s Metal capabilities, even generating Metal shaders on the fly to emulate fixed-function behavior. Our design overcomes current limitations – for instance, older fixed-function-heavy games that Apple’s own toolkit cannot run well can fall back to the DXVK path, ensuring broad compatibility. At the same time, we incorporate optimizations like MSync to reduce overhead from synchronization and Metal-specific tuning to maximize the Apple GPU’s throughput.

 

In essence, this translation layer serves as a bridge between two worlds: it convinces DirectX 9 games they’re running on a Windows PC with a D3D9 GPU, while actually executing on an ARM64 Mac with a Metal GPU. With careful engineering, the performance penalty for this trickery can be kept very low – often on the order of a few percent – making gameplay smooth and responsive. This means classics from the DirectX 9 era (and even many later titles) can be enjoyed on Apple Silicon machines at high frame rates and fidelity, without requiring a separate Windows installation or heavy virtualization. By pushing the boundaries of current solutions and introducing targeted innovations, our approach aims to meet or exceed the state-of-the-art, delivering both speed and compatibility that rival native platforms.

 

Sources:

Apple support forum (Rosetta 2 not supporting 32-bit)

Phoronix news (Wine Hangover with FEX/Box64 for x86 on ARM)

DXVK project documentation (D3D9 fixed-function shader generation)

Developer blog on CrossOver and Apple GPTK (D3DMetal vs DXVK for DX9, MSync)

Sources
Using everything you know create a detailed plan of implementation for a D3D to Metal d3d9.dll that we will use with wow64 to get great performance. The first game I wish to test is Fallout New Vegas. I have gotten the game running on DXVK - Molten - Metal, but due to the game having 1000ish draw calls per frame which are treated as separate by the cpu, performance is terrible (around 20 fps). 

Going to direct to metal should drastically reduce the cpu overhead and make the game fly
Thought for 16m 50s

Below is a concrete, “you could actually build this” architectural plan for a Direct3D 9 → Metal drop‑in d3d9.dll designed specifically for Apple Silicon macOS and 32‑bit DX9 games (Fallout: New Vegas first) running under Wine WoW64.

 

The main performance thesis is:

The killer overhead in your current chain is not “1000 draws is too many” (1000 draws/frame is normal for DX9-era engines), it’s that each draw (and the many state calls around it) becomes a pile of translated work through DXVK → Vulkan → MoltenVK → Metal, plus WoW64/emulation boundary costs, plus extra validation and sync.

A fast solution looks less like “call Metal once per D3D call” and more like what real drivers do: front-end records a compact command stream, back-end replays it efficiently, state changes are deduped, and resource binding is amortized using Metal features like argument buffers (bind lots of resources with fewer CPU calls) and (optionally) indirect command buffers .

0) Ground rules and key constraints
Target runtime environment

32-bit Windows game (FNV) under Wine WoW64 on macOS ARM64.

You want a replacement d3d9.dll that ultimately submits Metal work.

Wine has a well-established mechanism for “Windows PE DLL + native Unix sidecar library” (a Wine builtin is “a Win32 dll encapsulated inside a Unix library”) , and modern WoW64 support allows 32‑bit PE modules to call 64‑bit Unix libraries via thunks/dispatch mechanisms .

Reality check: D3DMetal doesn’t save you for DX9

On mac gaming stacks today, DX9 commonly falls back to DXVK→MoltenVK→Metal because Apple’s D3DMetal is aimed at newer DirectX (DX11/12) in practice, so DX9 is exactly the gap you’re filling .

1) The performance architecture you actually want

If you build a naïve D3D9→Metal wrapper where every COM call crosses into native Metal code, you’ll still drown in CPU overhead because D3D9 is extremely chatty (lots of SetRenderState / SetTexture / SetSamplerState between draws).

The “driver-like” architecture

Split into two layers inside d3d9.dll:

Front-end (32-bit PE, runs where the game runs)

Implements all D3D9 COM interfaces (IDirect3D9, IDirect3DDevice9, resources, shaders…).

Does almost no heavy work.

Maintains a shadow copy of device state (render states, samplers, textures, shaders, vertex decl, stream sources, constants).

Writes rendering work into a shared command stream ring buffer as compact packets.

Only forces synchronization on hard boundaries (e.g., Present, GetQueryData, Lock readback cases).

Back-end (64-bit “unixlib” / native side, ARM64)

Owns Metal device, queues, pipeline caches, heaps, command buffers.

Reads command stream packets and replays them into Metal with aggressive:

state dedupe

pipeline caching

resource binding amortization

batched encoders / minimized pass breaks

This is conceptually similar to Wine’s own “command stream” approach used in wined3d to decouple app thread from driver work (Wine has long-running work around wined3d command streams and enabling MT command streams by default) —but you’re taking it further by also reducing WoW64 boundary churn.

Why this helps your “1000 draw calls = 20 fps” scenario

Even if FNV is “only” ~1000 draws, the real number of D3D9 calls per frame may be 10k–50k once you count state and resource chatter. With the architecture above:

Many state calls become front-end only (update shadow state, no native call).

The back-end sees a much smaller stream: “draw with state snapshot X”, “update buffer region”, “present”.

You can make per-draw replay very tight on the ARM64 side.

2) Exact binary layout: how to ship it as d3d9.dll under WoW64
Recommended packaging

d3d9.dll (PE32): implements the full COM surface for 32-bit games; mostly writes packets to ring buffer.

d3d9.dll (PE64)

and a minimal packet list for FNV-first support (so you don’t overbuild D3D9 you don’t need).