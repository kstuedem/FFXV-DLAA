#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <com_ptr.hpp>
#include <crc32_hash.hpp>
#include <d3d11.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_helpers.h>
#include <set>

#include "intermediate/PrepareMotionVectors.h"
#include "intermediate/0x0D1CD1AA.h"

using namespace reshade::api;

NVSDK_NGX_Parameter* capabilityParameters = nullptr;
NVSDK_NGX_Parameter* parameters = nullptr;
NVSDK_NGX_Handle* dlssHandle = nullptr;

com_ptr<ID3D11Texture2D> motionVectorTexture;
com_ptr<ID3D11UnorderedAccessView> motionVectorUAV;
com_ptr<ID3D11ComputeShader> prepareMotionVectorShader;

com_ptr<ID3D11Buffer> cbSharpenModify;

std::set<ID3D11PixelShader*> taaShaders;
std::set<ID3D11PixelShader*> sharpenShaders;

bool dlssAvailable = false;
uint32_t shaderHash;
uint32_t currentWidth = 0;
uint32_t currentHeight = 0;
void* mappedConstantBuffer = nullptr;
float jitter[] = { 0.0f, 0.0f };
bool invokedThisFrame = false;
bool needReset = false;
bool needReinitialize = false;
NVSDK_NGX_DLSS_Hint_Render_Preset preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
float sharpenMultiplier = 1.0f;

void ReleaseDLSS() {
    if (parameters) {
        NVSDK_NGX_D3D11_DestroyParameters(parameters);
        parameters = nullptr;
    }
    if (dlssHandle) {
        NVSDK_NGX_D3D11_ReleaseFeature(dlssHandle);
        dlssHandle = nullptr;
    }
    motionVectorTexture.reset();
    motionVectorUAV.reset();
}

void Cleanup() {
    ReleaseDLSS();

    prepareMotionVectorShader.reset();

    if (capabilityParameters) {
        NVSDK_NGX_D3D11_DestroyParameters(capabilityParameters);
        capabilityParameters = nullptr;
    }
}

static void drawSettings(reshade::api::effect_runtime*)
{
    if (ImGui::DragFloat("Sharpen Amount", &sharpenMultiplier, 0.1f, 0.0f, 2.0f)) {
        reshade::set_config_value(nullptr, "DLAA", "SharpenAmount", sharpenMultiplier);
    }

    static const NVSDK_NGX_DLSS_Hint_Render_Preset presets[] = {
        NVSDK_NGX_DLSS_Hint_Render_Preset_K,
        NVSDK_NGX_DLSS_Hint_Render_Preset_M
    };
    static const char* presetNames[] = {
        "Preset K(DLSS 4.0)",
        "Preset M(DLSS 4.5)"
    };
    const char* combo_preview_value = preset == NVSDK_NGX_DLSS_Hint_Render_Preset_K ? presetNames[0] : presetNames[1];
    if (ImGui::BeginCombo("Preset", combo_preview_value, 0))
    {
        for (int i = 0; i < 2; i++) {
            bool isSelected = preset == presets[i];
            if (ImGui::Selectable(presetNames[i], isSelected)) {
                preset = presets[i];
                needReinitialize = true;
                reshade::set_config_value(nullptr, "DLAA", "Preset", preset);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void OnInitDevice(reshade::api::device* device) {
    reshade::get_config_value(nullptr, "DLAA", "SharpenAmount", sharpenMultiplier);
    if (sharpenMultiplier < 0.0f || sharpenMultiplier > 2.0f) {
        sharpenMultiplier = 1.0f;
    }
    int presetInt;
    reshade::get_config_value(nullptr, "DLAA", "Preset", presetInt);
    preset = (NVSDK_NGX_DLSS_Hint_Render_Preset)presetInt;
    if (preset != NVSDK_NGX_DLSS_Hint_Render_Preset_K &&
        preset != NVSDK_NGX_DLSS_Hint_Render_Preset_M) {
        preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    }

    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init(1,
        L"",
        (ID3D11Device*)device->get_native());

    if (NVSDK_NGX_FAILED(NVSDK_NGX_Result_Success)) {
        return;
    }

    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilityParameters);
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Result_Success)) {
        return;
    }

    int needsUpdatedDriver = 0;
    int dlssSupported = 0;
    int featureInitResult = 0;
    if(NVSDK_NGX_FAILED(capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver)) ||
        needsUpdatedDriver ||
        NVSDK_NGX_FAILED(capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssSupported)) ||
        !dlssSupported ||
        NVSDK_NGX_FAILED(capabilityParameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &featureInitResult)) ||
        !featureInitResult) {
        NVSDK_NGX_D3D11_DestroyParameters(capabilityParameters);
        capabilityParameters = nullptr;
        return;
    }
    dlssAvailable = true;

    ID3D11Device* d3d11Device = (ID3D11Device*)device->get_native();

    d3d11Device->CreateComputeShader(__PrepareMotionVectors.data(),
        __PrepareMotionVectors.size_bytes(),
        nullptr,
        &prepareMotionVectorShader);

    {
        D3D11_BUFFER_DESC bufferDesc;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.ByteWidth = 16;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufferDesc.MiscFlags = 0;
        bufferDesc.StructureByteStride = 0;
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        HRESULT hr = d3d11Device->CreateBuffer(&bufferDesc, nullptr, &cbSharpenModify);
    }
}

void OnDestroyDevice(reshade::api::device* device) {
    Cleanup();
}

bool OnCreatePipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout layout,
    uint32_t subobjectCount,
    const reshade::api::pipeline_subobject* subobjects) {
    bool replacedShader = false;

    for (uint32_t i = 0; i < subobjectCount; ++i) {
        if (subobjects[i].type != reshade::api::pipeline_subobject_type::pixel_shader) {
            continue;
        }

        shader_desc* desc = (shader_desc*)subobjects[i].data;
        shaderHash = compute_crc32((const uint8_t*)desc->code, desc->code_size);

        if (shaderHash == 0x0D1CD1AA) {
            desc->code = __0x0D1CD1AA.data();
            desc->code_size = __0x0D1CD1AA.size_bytes();
            replacedShader = true;
        }
    }

    return replacedShader;
}

void OnInitPipeline(device* device,
    pipeline_layout layout,
    uint32_t subobjectCount,
    const pipeline_subobject* subobjects,
    pipeline pipeline) {
    for (uint32_t i = 0; i < subobjectCount; ++i) {
        if (subobjects[i].type != reshade::api::pipeline_subobject_type::pixel_shader) {
            continue;
        }

        if (shaderHash == 0x0DF0A97D) {
            taaShaders.insert((ID3D11PixelShader*)pipeline.handle);
        }
        if (shaderHash == 0x0D1CD1AA) {
            sharpenShaders.insert((ID3D11PixelShader*)pipeline.handle);
        }
    }
}

void OnDestroyPipeline(reshade::api::device* device, reshade::api::pipeline pipeline) {
    if (taaShaders.find((ID3D11PixelShader*)pipeline.handle) != taaShaders.end()) {
        taaShaders.erase((ID3D11PixelShader*)pipeline.handle);
    }
    if (sharpenShaders.find((ID3D11PixelShader*)pipeline.handle) != sharpenShaders.end()) {
        sharpenShaders.erase((ID3D11PixelShader*)pipeline.handle);
    }
}

bool OnDraw(reshade::api::command_list* cmd_list, 
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance) {
    ID3D11DeviceContext* deviceContext = (ID3D11DeviceContext*)(cmd_list->get_native());
    com_ptr<ID3D11PixelShader> shader;
    deviceContext->PSGetShader(&shader, nullptr, nullptr);

    if (dlssAvailable && taaShaders.find(shader.get()) != taaShaders.end()) {
        com_ptr<ID3D11RenderTargetView> renderTargetView;
        deviceContext->OMGetRenderTargets(1, &renderTargetView, nullptr);

        if (!renderTargetView) {
            return false;
        }

        com_ptr<ID3D11Resource> renderTargetResource;
        renderTargetView->GetResource(&renderTargetResource);

        com_ptr<ID3D11Texture2D> renderTargetTexture;
        renderTargetResource->QueryInterface(&renderTargetTexture);

        D3D11_TEXTURE2D_DESC renderTargetDesc;
        renderTargetTexture->GetDesc(&renderTargetDesc);
        
        uint32_t width = renderTargetDesc.Width;
        uint32_t height = renderTargetDesc.Height;

        if (currentWidth != width ||
            currentHeight != height ||
            needReinitialize) {
            ReleaseDLSS();
        }

        if (!dlssHandle) {
            NVSDK_NGX_D3D11_AllocateParameters(&parameters);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Width, width);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Height, height);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutWidth, width);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_OutHeight, height);
            NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_DLAA);
            NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_IsHDR);
            NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
            NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
            NVSDK_NGX_Result result = NVSDK_NGX_D3D11_CreateFeature(deviceContext, NVSDK_NGX_Feature_SuperSampling, parameters, &dlssHandle);
            if (NVSDK_NGX_FAILED(result)) {
                dlssHandle = nullptr;
                dlssAvailable = false;
                return false;
            }

            com_ptr<ID3D11Device> device;
            deviceContext->GetDevice(&device);

            {
                D3D11_TEXTURE2D_DESC motionVectorDesc;
                motionVectorDesc.Width = width;
                motionVectorDesc.Height = height;
                motionVectorDesc.Usage = D3D11_USAGE_DEFAULT;
                motionVectorDesc.ArraySize = 1;
                motionVectorDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
                motionVectorDesc.SampleDesc.Count = 1;
                motionVectorDesc.SampleDesc.Quality = 0;
                motionVectorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                motionVectorDesc.CPUAccessFlags = 0;
                motionVectorDesc.MiscFlags = 0;
                motionVectorDesc.MipLevels = 1;

                device->CreateTexture2D(&motionVectorDesc,
                    nullptr,
                    &motionVectorTexture);
            }
            {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
                uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = 0;

                device->CreateUnorderedAccessView(motionVectorTexture.get(),
                    &uavDesc,
                    &motionVectorUAV);
            }

            currentWidth = width;
            currentHeight = height;
            needReinitialize = false;
        }

        com_ptr<ID3D11Buffer> cbTemporalAA;
        deviceContext->PSGetConstantBuffers(0, 1, &cbTemporalAA);

        com_ptr<ID3D11ShaderResourceView> inColorSRV;
        deviceContext->PSGetShaderResources(0, 1, &inColorSRV);

        com_ptr<ID3D11ShaderResourceView> inDepthSRV;
        deviceContext->PSGetShaderResources(3, 1, &inDepthSRV);

        com_ptr<ID3D11ShaderResourceView> inVelocitySRV;
        deviceContext->PSGetShaderResources(6, 1, &inVelocitySRV);

        if (!cbTemporalAA ||
            !inColorSRV ||
            !inDepthSRV ||
            !inVelocitySRV) {
            return false;
        }

        com_ptr<ID3D11Resource> inColor;
        inColorSRV->GetResource(&inColor);
        com_ptr<ID3D11Resource> inDepth;
        inDepthSRV->GetResource(&inDepth);

        {
            ID3D11ShaderResourceView* srvs[] = { inVelocitySRV.get() , inDepthSRV.get() };
            ID3D11UnorderedAccessView* uavs[] = { motionVectorUAV.get() };
            ID3D11Buffer* cbs[] = { cbTemporalAA.get() };
            deviceContext->CSSetShader(prepareMotionVectorShader.get(), 0, 0);
            deviceContext->CSSetShaderResources(0, 2, srvs);
            deviceContext->CSSetConstantBuffers(0, 1, cbs);
            deviceContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            deviceContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            srvs[0] = nullptr;
            srvs[1] = nullptr;
            deviceContext->CSSetShaderResources(0, 2, srvs);
        }

        {
            NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams = {};
            evalParams.Feature.pInColor = inColor.get();
            evalParams.Feature.pInOutput = renderTargetResource.get();
            evalParams.pInDepth = inDepth.get();
            evalParams.pInMotionVectors = motionVectorTexture.get();
            evalParams.InJitterOffsetX = jitter[0] * width;
            evalParams.InJitterOffsetY = jitter[1] * height;
            evalParams.InReset = needReset ? 1 : 0;
            evalParams.InMVScaleX = 1.0f;
            evalParams.InMVScaleY = 1.0f;
            evalParams.InRenderSubrectDimensions.Width = width;
            evalParams.InRenderSubrectDimensions.Height = height;
            NVSDK_NGX_Result res = NGX_D3D11_EVALUATE_DLSS_EXT(deviceContext, dlssHandle, parameters, &evalParams);
        }

        invokedThisFrame = true;
        return true;
    }
    else if (sharpenShaders.find(shader.get()) != sharpenShaders.end()) {
        {
            D3D11_MAPPED_SUBRESOURCE mappedResource;
            if (SUCCEEDED(deviceContext->Map(cbSharpenModify.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
                ((float*)mappedResource.pData)[0] = sharpenMultiplier;
                deviceContext->Unmap(cbSharpenModify.get(), 0);
            }
        }

        ID3D11Buffer* cbs[] = { cbSharpenModify.get() };
        deviceContext->PSSetConstantBuffers(1, 1, cbs);
    }
    return false;
}

void OnMapBufferRegion(
    device* device,
    resource resource,
    uint64_t offset,
    uint64_t size,
    map_access access,
    void** mapped_data) {
    if (access != map_access::write_discard) {
        return;
    }
    D3D11_BUFFER_DESC bd;
    ((ID3D11Buffer*)resource.handle)->GetDesc(&bd);
    if (bd.ByteWidth == 256) {
        mappedConstantBuffer = *mapped_data;
    }
}

void OnUnmapBufferRegion(
    device* device,
    resource resource) {
    if (mappedConstantBuffer) {
        jitter[0] = ((float*)mappedConstantBuffer)[8];
        jitter[1] = ((float*)mappedConstantBuffer)[9];
        mappedConstantBuffer = nullptr;
    }
}

void OnPresent(command_queue* queue,
    swapchain* swapchain,
    const rect* source_rect,
    const rect* dest_rect,
    uint32_t dirty_rect_count,
    const rect* dirty_rects) {
    needReset = !invokedThisFrame;
    invokedThisFrame = false;
}

extern "C" __declspec(dllexport) const char *NAME = "FFXV DLAA";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Replaces TAA with DLAA";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
        reshade::register_overlay(nullptr, drawSettings);
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::register_event < reshade::addon_event::create_pipeline>(OnCreatePipeline);
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::map_buffer_region>(OnMapBufferRegion);
        reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion);
        reshade::register_event<reshade::addon_event::present>(OnPresent);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
