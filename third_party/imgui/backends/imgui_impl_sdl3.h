#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Gamepad;
typedef union SDL_Event SDL_Event;

IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForOpenGL(SDL_Window* window, void* sdl_gl_context);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForVulkan(SDL_Window* window);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForD3D(SDL_Window* window);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForMetal(SDL_Window* window);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForSDLRenderer(SDL_Window* window, SDL_Renderer* renderer);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForSDLGPU(SDL_Window* window);
IMGUI_IMPL_API bool     ImGui_ImplSDL3_InitForOther(SDL_Window* window);
IMGUI_IMPL_API void     ImGui_ImplSDL3_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplSDL3_NewFrame();
IMGUI_IMPL_API bool     ImGui_ImplSDL3_ProcessEvent(const SDL_Event* event);

enum ImGui_ImplSDL3_GamepadMode { ImGui_ImplSDL3_GamepadMode_AutoFirst, ImGui_ImplSDL3_GamepadMode_AutoAll, ImGui_ImplSDL3_GamepadMode_Manual };
IMGUI_IMPL_API void     ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode mode, SDL_Gamepad** manual_gamepads_array = nullptr, int manual_gamepads_count = -1);

enum ImGui_ImplSDL3_MouseCaptureMode { ImGui_ImplSDL3_MouseCaptureMode_Enabled, ImGui_ImplSDL3_MouseCaptureMode_EnabledAfterDrag, ImGui_ImplSDL3_MouseCaptureMode_Disabled };
IMGUI_IMPL_API void     ImGui_ImplSDL3_SetMouseCaptureMode(ImGui_ImplSDL3_MouseCaptureMode mode);

#endif
