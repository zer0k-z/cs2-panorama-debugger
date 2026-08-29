# CS2 Panorama Debugger

**NOTE: Vibe coded this plugin. Big time.**

Press **F6** to toggle Valve's own Panorama debugger like the way Dota does it.

CS2 ships the whole debugger and never builds it. This Metamod plugin builds it. Windows only.

## Install

Download the latest build, then:

- `addons/cs2-panorama-debugger/bin/win64/cs2-panorama-debugger.dll` → `csgo/addons/…`
- `addons/metamod/cs2-panorama-debugger.vdf` → `csgo/addons/metamod/`

Start the game with `-insecure` so Metamod is loaded into `cs2.exe`, and `meta list` should
show the plugin. Press **F6**.

Everything the plugin does prints to the console prefixed `[panorama-debugger]`, including a warning
naming the pattern if one stops matching after a game update.

## Notes

- **`hittest="false"` panels cannot be picked.** That is panorama behaving correctly, not a bug —
  Inspect hit-tests, and those panels opt out.
- **Inspect needs something to seed from.** It reads the game window's current mouse hover, so with
  nothing hoverable under the cursor the tree comes up empty. Press Inspect, then click the game
  window to focus it.
- **While the debugger is open the game is deny-listed for input** until you pick something. That is
  what `k_EGameInputCaptureAll` means.
- **F6 hides the window rather than destroying it**, and one window is leaked at unload. engine2 has
  no way to withdraw a panorama view once `AddPanoramaView` has taken one — it only appends to
  `m_Views` and clears the whole vector at shutdown — so destroying the window would leave the render
  walking freed memory. The debugger therefore keeps its state across F6.
- **The Vulkan backend submits on the game's own graphics queue**, safe only because the present hook
  runs on the thread that owns it.

## Build

```
xmake f -p windows -a x64 -m release
xmake
```

Needs `hl2sdk-cs2` and `metamod-source`. Both default to the checkouts under
`C:/coding/cs2kz-metamod`; override with `HL2SDKCS2` and `MMSOURCE`.

The plugin uses no SDK type beyond `Msg()`, `ISmmPlugin` and `IVEngineServer2::ServerCommand`. That
last one drags in `eiface.h`, which includes `network_connection.pb.h` — the only protobuf involved.
No type from it is ever used, so nothing is compiled or linked; the header is vendored in
`protobuf/` so the build needs neither protoc nor a protobuf library.

## Dota vs CS2 vs this plugin

|  | Dota | CS2 | This plugin |
|---|---|---|---|
| **Enabling** | `Init` computes a debugger-enable bool and registers five events, including `CreateDebuggerWindow` | `Init` hard-codes the enable argument to `0` and registers no debugger events | Skips the event system and calls the builder directly |
| **Building it** | `OnCreateDebuggerWindow` does the whole thing | No such function — compiled out | Reconstructs that caller step by step |
| **Rendering** | Its own frame step: a scene view with `"outputcolor"` bound to the debugger's swapchain | No `"outputcolor"` anywhere in engine2; panorama renders only inside `CGameUIService`, against the main swapchain | Observes the render target and presents the window itself |
| **Input** | Attached OS window + the handler's own panorama input context | Neither route reaches a window engine2 did not create | Builds `InputMessage_t` events in its own window procedure |
| **Cursor** | Comes with the input context | Held by gameplay until something asks for it back | `AddGameInputHandler` — the call chat and the main menu make |

### Enabling

`CDebugger`, `CPanoramaClientDebugger` and every event registration still ship in
`panoramauiclient.dll`; `debugger.vxml_c` and `debugger.vcss_c` still ship in `pak01`. Only the
caller was removed. `ToggleDebugger` still exists in `panorama.dll` and still dispatches
`CreateDebuggerWindow` — into the void, because nothing is listening. That is why pressing the key
Valve's own code binds does nothing.

### Building it

Dota's `OnCreateDebuggerWindow` reads `panorama_debugger.cfg`, creates an OS window, attaches it to
the input system, creates a render-system swapchain for it, calls `CreateNewUILayerWindow` with the
handler's own panorama input context, calls `CreateDebugger`, and restores the saved splitter
position.

The F6 sequence is that function, minus what CS2 cannot support and plus what CS2 needs:

1. `Plat_CreateWindow` — with Dota's own window flags
2. `CreateNewOffscreenUIWindow(w, h, name, /*context*/ nullptr, /*bDrawToBackBuffer*/ false)`
3. `SetWindowScaleFactor(1.0f)` then `OnWindowResize(w, h)` — **before** anything else
4. `SetPlatWindow`, `CreateDebugger`, `Activate`
5. `AddPanoramaView`
6. `SetVisible(true)`
7. Build the render path
8. `AddGameInputHandler`

Three of those are non-obvious:

- **Offscreen, not a UI layer.** The last argument reaches `BInitializeSurface`, and the render path
  ANDs `panorama_use_backbuffer_directly` with that *per-window* flag. `CreateNewUILayerWindow`
  hardcodes it to `1`, so through it the only way to get an off-backbuffer render target is to force
  the convar globally — which turns the entire game's UI upside down. The offscreen creator passes
  the flag through, so the game keeps rendering normally.
- **Scale factor before the view.** `AddPanoramaView` early-outs unless the surface has a size *and*
  a non-zero scale factor. The offscreen creator leaves the scale at zero, so without step 3 the
  window silently never becomes a view.
- **A null input context.** One of our own sits above the game's "Panorama UI" in the input stack,
  and engine2 then routes the *game* window's mouse into our window: the debugger scrolls while you
  drag over the game, and Inspect cannot hit-test anything the game owns.

### Rendering

Dota renders the debugger as its own frame step: a scene view named `"Panorama Debugger"` with
`"outputcolor"` bound to the debugger's swapchain.

CS2's engine2 does not contain the string `"outputcolor"` at all. Its panorama views are rendered
only from inside `CGameUIService`'s render, through `AddPanoramaViewsToScene`, against the main
window's swapchain — and calling that a second time in a frame is **not safe**. It faults inside
panorama's per-surface render-target machinery, and does so identically when handed the game's own
swapchain, so the target is not the problem: the second call is.

So the plugin works one level down, in the render backend: find the surface the debugger draws into,
copy it into a swapchain of our own, and present that at Source2's `CRenderDeviceBase::Present` —
the same vtable slot in both backends, and the point in the frame where the image is finished.

Finding the surface differs by backend, because CS2 picks its render system at startup and the tools
launcher forces Vulkan (`-gpuraytracing` wins over `-dx11`):

- **D3D11** — hook `ID3D11DeviceContext::OMSetRenderTargets` and record the targets the engine binds.
  Substitution was tried first and does not work: panorama's draws do not run on the thread that runs
  the layer render, so a flag set around that call sees no binds at all, and making the flag global
  hijacked every thread's render target and killed the display driver.
- **Vulkan** — attachments are bound inside a command buffer, so there is nothing equivalent to
  watch. The target is identified when it is *created* instead, by hooking the module's own
  `vkCreateImage` pointer.

Either way the target is matched by size, remembering that panorama rounds each dimension **up to a
multiple of 32** and pools the result. Two consequences worth knowing:

- Only the window-sized region of the target is copied. Blitting the padding as well squashes the
  image, and input — which passes raw window coordinates — then picks a different element than the
  one under the cursor.
- There is deliberately no "smallest target that covers the window" fallback. It matches the game's
  own layers just as readily and puts the HUD in the debugger window.

### Input and the cursor

Dota gets input for free from the two calls in its window creation. In CS2 neither helps:
`IInputSystem::AttachToWindow` only moves OS focus and delivers nothing to a panorama window, and a
pushed input context does the wrong thing as described above. engine2 pumps and routes input only for
windows it created.

So the window procedure is subclassed and `panorama::InputMessage_t` events are built and handed to
the window's `CUIWindowInput` directly — mouse moves, buttons, wheel, key down/up, and `WM_CHAR` as
`k_eKeyChar`, which is the one a text entry actually reads.

The cursor is separate. While the game holds mouse capture it hides and re-centres the OS pointer, so
the window is unusable with a mouse until something asks for it back. `AddGameInputHandler` with
`k_EGameInputCaptureAll` is what the game's own UIs use, and it is what Inspect needs too — Inspect
seeds itself from the *game* window's current mouse hover, and during gameplay there isn't one.

Capture follows focus, so nothing has to be toggled by hand:

- focusing the debugger window re-takes it, because the window is useless without it
- a mouse-down on a window that is not ours, while the game is foreground, is a **pick** — the
  capture is released and the game is immediately playable again

### One more thing engine2 does for free that CS2 does not

`AddPanoramaView` also joins the window to engine2's *input dispatch*: `RecalculateInputOrder` builds
`m_vecWindowInputOrder` from `m_Views`, whatever context the window was created with. engine2 then
offers each window the event until one handles it, so a click on empty space in the game falls
through to the debugger — clicking the top-left of the screen presses its Inspect button. The plugin
takes itself back out of that list every frame, because engine2 rebuilds it whenever the views change.
