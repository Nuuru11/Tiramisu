// One-shot hardware draw. Keep the launcher's CoreWindow swapchain intact.
#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <array>
#include <chrono>
#include <string>
#include <winrt/base.h>

namespace longan {
struct triangle_probe {
  static constexpr UINT size = 128;
  std::array<uint32_t, size * size> pixels{};
  std::chrono::steady_clock::time_point preview_until{};

  HRESULT present_hr = E_PENDING;

  std::string render(IDXGIFactory2* factory) {
    preview_until = {};
    present_hr = E_PENDING;
    const char* stage = "Create device";
    try {
      using winrt::com_ptr;
      using winrt::check_hresult;
      com_ptr<ID3D12Device> device;
      check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
      stage = "Device OK / prepare pipeline";
      com_ptr<ID3D12CommandQueue> queue;
      D3D12_COMMAND_QUEUE_DESC q{};
      check_hresult(device->CreateCommandQueue(&q, IID_PPV_ARGS(queue.put())));
      D3D12_ROOT_SIGNATURE_DESC desc{};
      com_ptr<ID3DBlob> serialized, vs, ps;
      check_hresult(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), nullptr));
      com_ptr<ID3D12RootSignature> root;
      check_hresult(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
      constexpr char shader[] =
          "float4 vs(uint i:SV_VertexID):SV_Position {float2 p[3]={float2(-.8,-.8),float2(0,.8),float2(.8,-.8)};return float4(p[i],0,1);}"
          "float4 ps():SV_Target{return float4(0,1,0,1);}";
      check_hresult(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,vs.put(),nullptr));
      check_hresult(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,ps.put(),nullptr));
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
      pipeline.pRootSignature = root.get();
      pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      auto& blend = pipeline.BlendState.RenderTarget[0];
      blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
      blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
      blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      blend.LogicOp = D3D12_LOGIC_OP_NOOP;
      blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      pipeline.SampleMask = UINT_MAX;
      pipeline.SampleDesc.Count = 1;
      pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pipeline.RasterizerState.DepthClipEnable = TRUE;
      pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pipeline.NumRenderTargets = 1;
      pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
      com_ptr<ID3D12PipelineState> pso;
      check_hresult(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(pso.put())));
      D3D12_RESOURCE_DESC target{};
      target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      target.Width = target.Height = size;
      target.DepthOrArraySize = target.MipLevels = target.SampleDesc.Count = 1;
      target.Format = pipeline.RTVFormats[0];
      target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      heap.CreationNodeMask = heap.VisibleNodeMask = 1;
      com_ptr<ID3D12Resource> texture, buffer;
      // A separate queue-backed composition chain lets us exercise DX12 Present
      // without destroying the CoreWindow chain used by the launcher.
      com_ptr<IDXGISwapChain1> test_chain;
      DXGI_SWAP_CHAIN_DESC1 swap_desc{};
      swap_desc.Width = swap_desc.Height = size;
      swap_desc.Format = target.Format;
      swap_desc.SampleDesc.Count = 1;
      swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      swap_desc.BufferCount = 2;
      swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      present_hr = factory ? factory->CreateSwapChainForComposition(queue.get(), &swap_desc,
          nullptr, test_chain.put()) : E_NOINTERFACE;
      if (SUCCEEDED(present_hr)) {
        check_hresult(test_chain->GetBuffer(0, IID_PPV_ARGS(texture.put())));
      } else {
        check_hresult(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(texture.put())));
      }
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      UINT64 bytes;
      device->GetCopyableFootprints(&target,0,1,0,&footprint,nullptr,nullptr,&bytes);
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC readback{};
      readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      readback.Width = bytes;
      readback.Height = readback.DepthOrArraySize = readback.MipLevels = readback.SampleDesc.Count = 1;
      readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      check_hresult(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&readback,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(buffer.put())));
      D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
      descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      descriptors.NumDescriptors = 1;
      com_ptr<ID3D12DescriptorHeap> rtv;
      check_hresult(device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(rtv.put())));
      auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(texture.get(),nullptr,handle);
      com_ptr<ID3D12CommandAllocator> alloc;
      com_ptr<ID3D12GraphicsCommandList> list;
      check_hresult(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(alloc.put())));
      check_hresult(device->CreateCommandList(0,q.Type,alloc.get(),pso.get(),IID_PPV_ARGS(list.put())));
      float black[4] = {0,0,0,1};
      D3D12_VIEWPORT viewport{0,0,size,size,0,1};
      D3D12_RECT rect{0,0,size,size};
      D3D12_RESOURCE_BARRIER barrier{};
      if (test_chain) {
        barrier.Transition = {texture.get(), 0, D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RENDER_TARGET};
        list->ResourceBarrier(1, &barrier);
      }
      list->RSSetViewports(1,&viewport);
      list->RSSetScissorRects(1,&rect);
      list->OMSetRenderTargets(1,&handle,FALSE,nullptr);
      list->ClearRenderTargetView(handle,black,0,nullptr);
      list->SetGraphicsRootSignature(root.get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3,1,0,0);
      barrier.Transition = {texture.get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
      list->ResourceBarrier(1,&barrier);
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = texture.get();
      dst.pResource = buffer.get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprint;
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      if (test_chain) {
        barrier.Transition = {texture.get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_PRESENT};
        list->ResourceBarrier(1, &barrier);
      }
      check_hresult(list->Close());
      com_ptr<ID3D12Fence> fence;
      check_hresult(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.put())));
      stage = "Device OK / submit";
      ID3D12CommandList* commands[] = {list.get()};
      queue->ExecuteCommandLists(1,commands);
      check_hresult(queue->Signal(fence.get(),1));
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!fence->GetCompletedValue() && std::chrono::steady_clock::now() < deadline) Sleep(1);
      if (fence->GetCompletedValue() != 1) return "Device OK / submitted / GPU did not complete";
      stage = "Device OK / submitted / read pixels";
      void* data{};
      check_hresult(buffer->Map(0,nullptr,&data));
      for (UINT y=0; y<size; ++y)
        memcpy(pixels.data()+y*size, static_cast<const char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch,size*4);
      D3D12_RANGE written{};
      buffer->Unmap(0,&written);
      if (pixels[size*size/2+size/2] != 0xff00ff00 || pixels.front() != 0xff000000)
        return "Device OK / submitted / triangle pixel check failed";
      if (test_chain) present_hr = test_chain->Present(0, 0);
      return {};
    } catch (const winrt::hresult_error& error) {
      char text[128];
      snprintf(text,sizeof(text),"%s failed (0x%08X)",stage,static_cast<unsigned>(error.code()));
      return text;
    }
  }
};
}
