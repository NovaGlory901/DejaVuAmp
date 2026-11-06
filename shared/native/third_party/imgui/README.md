# Dear ImGui

Dear ImGui 1.92.9 (git tag `v1.92.9`, commit `01380c579715e62fb9a8d6ec0502c4ea83bfde6e`) by Omar Cornut, https://github.com/ocornut/imgui, MIT licence (`LICENSE.txt`).
Copied unchanged: the core (`imgui*.cpp/.h`, `imstb_*.h`, `imconfig.h`) and the two backends the program uses, `backends/imgui_impl_sdl2.*` (window and input) and
`backends/imgui_impl_opengl3.*` (drawing; built with `IMGUI_IMPL_OPENGL_ES2` so it runs on OpenGL ES 2 like the rest of the program). The demo window
(`imgui_demo.cpp`) is not included. It draws FalseHorizon's settings panel (`apps/panel.cpp`), and is planned for DejaVuAmp's window.
To update: copy the same files from a newer tag and re-run the tests.
