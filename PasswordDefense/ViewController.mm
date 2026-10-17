//
//  ViewController.mm
//  PasswordDefense
//
//  Created by spooks on 11/03/2026.
//

#import "ViewController.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include "GUI.h"
#include "save.h"
#include "favicon.h"

@implementation ViewController
{
    MTKView*            _mtkView;
    id<MTLDevice>       _device;
    id<MTLCommandQueue> _commandQueue;
    BOOL                _shuttingDown;
    BOOL                _minimizing;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    _shuttingDown = NO;
    _minimizing = NO;

    // Setup Metal
    _device = MTLCreateSystemDefaultDevice();
    _commandQueue = [_device newCommandQueue];

    // Setup MTKView
    _mtkView = [[MTKView alloc] initWithFrame:self.view.bounds device:_device];
    _mtkView.delegate = self;
    _mtkView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _mtkView.clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
    [self.view addSubview:_mtkView];

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Platform/Renderer backends
    ImGui_ImplOSX_Init(_mtkView);
    ImGui_ImplMetal_Init(_device);

    // Initialize config (paths, settings) — must be before GUIInit
    cfg::init();
    favicon::Init((__bridge void*)_device);

    // Initialize fonts, styles, and theme (from GUI.mm)
    render::GUIInit();
}

- (void)viewDidAppear
{
    [super viewDidAppear];
    // Set ourselves as the window delegate to handle close/minimize
    self.view.window.delegate = self;

    // Set default window size and center on screen
    NSWindow* window = self.view.window;
    [window setContentSize:NSMakeSize(WINDOW_WIDTH, WINDOW_HEIGHT)];
    [window center];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    // Handle resize if needed
}

- (void)drawInMTKView:(MTKView*)view
{
    if (_shuttingDown) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = view.bounds.size.width;
    io.DisplaySize.y = view.bounds.size.height;

    CGFloat framebufferScale = view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
    io.DisplayFramebufferScale = ImVec2(framebufferScale, framebufferScale);

    // Update clear color to match current ImGui WindowBg
    {
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        view.clearColor = MTLClearColorMake(bg.x, bg.y, bg.z, bg.w);
    }

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    MTLRenderPassDescriptor* renderPassDescriptor = view.currentRenderPassDescriptor;
    if (renderPassDescriptor == nil)
        return;

    // Start the Dear ImGui frame
    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_ImplOSX_NewFrame(_mtkView);
    ImGui::NewFrame();

    render::DisplayProgram();

    // Rendering
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, renderEncoder);
    [renderEncoder endEncoding];

    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];
}

- (void)shutdownImGui
{
    if (_shuttingDown) return;
    _shuttingDown = YES;
    _mtkView.paused = YES;
    favicon::Shutdown();
    render::GUIShutDown();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyPlatformWindows();
    ImGui::DestroyContext();
}

// NSWindowDelegate: handle window close cleanly
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    // If minimize-to-tray is enabled, hide window instead of closing
    if (render::IsMinimizeToTrayEnabled())
    {
        render::HideWindow();
        return NO;
    }

    [self shutdownImGui];
    return YES;
}


// NSWindowDelegate: track minimize so viewWillDisappear doesn't kill ImGui
- (void)windowWillMiniaturize:(NSNotification*)notification
{
    _minimizing = YES;
}

- (void)windowDidDeminiaturize:(NSNotification*)notification
{
    _minimizing = NO;
}

- (void)viewWillDisappear
{
    [super viewWillDisappear];
    // Don't shut down ImGui if we're just minimizing or hiding to tray
    if (!render::IsMinimizeToTrayEnabled() && !_minimizing)
        [self shutdownImGui];
}

@end
