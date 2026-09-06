#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "ChatTestSupport.h"
#include "Rendering/GraphicsBackend.h"
#include "Rendering/RenderFrameBuilder.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Camera.h"
#include "Runtime/Transform.h"
#include "SceneRendering/SceneRenderPass.h"
#include "UI/SpeechBubble.h"
#include "Gameplay/PlayerController.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
using ChatTestSupport::Fixture;
using ChatTestSupport::Require;
namespace Rendering = GameEngine::Rendering;
namespace Runtime = GameEngine::Runtime;

void SavePng(const Rendering::CapturedImage& image, const std::filesystem::path& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()))), "chat PNG factory could not be created");
    Require(SUCCEEDED(factory->CreateStream(stream.GetAddressOf())) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(image.width, image.height)),
        "chat PNG could not be initialized");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Require(SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA,
        "chat PNG encoder does not support BGRA pixels");
    // GPU readback is RGBA; this changes only byte order for WIC, never the rendered colors.
    std::vector<BYTE> bgra(image.pixels.size());
    for (std::size_t offset = 0; offset < bgra.size(); offset += 4)
    {
        bgra[offset] = std::to_integer<BYTE>(image.pixels[offset + 2]);
        bgra[offset + 1] = std::to_integer<BYTE>(image.pixels[offset + 1]);
        bgra[offset + 2] = std::to_integer<BYTE>(image.pixels[offset]);
        bgra[offset + 3] = std::to_integer<BYTE>(image.pixels[offset + 3]);
    }
    Require(SUCCEEDED(frame->WritePixels(image.height, image.width * 4,
        static_cast<UINT>(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit()), "chat PNG could not be written");
    std::cout << "  chat visual capture: " << path.string() << '\n';
}

Rendering::CapturedImage Capture(Fixture& fixture, Rendering::IGraphicsDevice& device,
    const std::filesystem::path& directory, const std::string& name)
{
    GameEngine::SceneRendering::SceneRenderPass frontend(fixture.cache);
    Rendering::RenderFrameBuilder builder;
    builder.SetRenderTargetSize({ fixture.width, fixture.height });
    frontend.Collect(fixture.game, builder);
    const auto frame = std::move(builder).Build();
    const auto& world = frame.GetDrawPackets(Rendering::RenderPass::Transparent);
    Require(!world.empty() && world.front().sortingOrder == -1000,
        "chat captures require the real background and game world");
    Require(!frame.GetDraws<Rendering::TextDraw>(Rendering::RenderPass::Overlay).empty(),
        "chat captures must use the real Korean font and text render path");
    Rendering::CapturedImage image;
    Require(device.RenderToImage(frame, image) && image.IsValid(), "chat scene GPU capture failed");
    SavePng(image, directory / (name + ".png"));
    return image;
}

void CheckChatPixels(Fixture& fixture, const Rendering::CapturedImage& baseline,
    const Rendering::CapturedImage& shown)
{
    const auto viewport = fixture.Component<Runtime::RectTransform>("ChatViewport").GetVisibleRect();
    std::size_t glyphPixels = 0;
    for (auto y = static_cast<unsigned int>(viewport.y); y < static_cast<unsigned int>(viewport.GetBottom()); ++y)
        for (auto x = static_cast<unsigned int>(viewport.x); x < static_cast<unsigned int>(viewport.GetRight()); ++x)
        {
            const auto index = (static_cast<std::size_t>(y) * shown.width + x) * 4;
            const int r = std::to_integer<int>(shown.pixels[index]);
            const int g = std::to_integer<int>(shown.pixels[index + 1]);
            const int b = std::to_integer<int>(shown.pixels[index + 2]);
            if (r > 170 && g > 170 && b > 170 &&
                (shown.pixels[index] != baseline.pixels[index] ||
                    shown.pixels[index + 1] != baseline.pixels[index + 1])) ++glyphPixels;
        }
    Require(glyphPixels > static_cast<std::size_t>(fixture.width) * fixture.height / 10000,
        "visible history must contain actual text pixels over the translucent chat panel");
}

void CheckHistoryClipping(Fixture& fixture, const Rendering::CapturedImage& emptyPanel,
    const Rendering::CapturedImage& history)
{
    const auto clip = fixture.Component<Runtime::RectTransform>("ChatViewport").GetVisibleRect();
    for (unsigned int y = 0; y < history.height; ++y)
        for (unsigned int x = 0; x < history.width; ++x)
        {
            if (clip.Contains(static_cast<float>(x) + .5f, static_cast<float>(y) + .5f)) continue;
            const auto index = (static_cast<std::size_t>(y) * history.width + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel)
                Require(history.pixels[index + channel] == emptyPanel.pixels[index + channel],
                    "a chat history glyph leaked outside its viewport into the panel padding or game world");
        }
}

void CheckNoticePixels(Fixture& fixture, const Rendering::CapturedImage& image)
{
    const auto clip = fixture.Component<Runtime::RectTransform>("ChatViewport").GetVisibleRect();
    std::size_t green = 0;
    std::size_t yellow = 0;
    for (auto y = static_cast<unsigned int>(clip.y); y < static_cast<unsigned int>(clip.GetBottom()); ++y)
        for (auto x = static_cast<unsigned int>(clip.x); x < static_cast<unsigned int>(clip.GetRight()); ++x)
        {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
            const int r = std::to_integer<int>(image.pixels[offset]);
            const int g = std::to_integer<int>(image.pixels[offset + 1]);
            const int b = std::to_integer<int>(image.pixels[offset + 2]);
            if (g > 170 && r < 165 && b < 180) ++green;
            if (r > 180 && g > 155 && b < 130) ++yellow;
        }
    Require(green > 20 && yellow > 20, "server notice glyphs did not render green and yellow");
}

void CheckSpeechPixels(Fixture& fixture, const Runtime::GameObject& bubble,
    const Rendering::CapturedImage& baseline, const Rendering::CapturedImage& shown, const bool wrapped)
{
    const Runtime::GameObject* body = nullptr;
    for (const auto* child : bubble.GetTransform().GetChildren())
        if (child->GetGameObject()->GetName() == "SpeechBubbleText") body = child->GetGameObject();
    Require(body != nullptr, "speech capture is missing its measured text child");
    const auto& text = *body->GetComponent<Runtime::TextRenderer>();
    const auto& rect = *body->GetComponent<Runtime::RectTransform>();
    const auto& camera = fixture.Component<Runtime::Camera>("MainCamera");
    const auto cameraPosition = camera.GetGameObject()->GetTransform().GetWorldPosition();
    const auto center = body->GetTransform().GetWorldPosition();
    const float worldPixels = static_cast<float>(fixture.height) / (2 * camera.GetOrthographicSize());
    const float x = static_cast<float>(fixture.width) * .5f + (center.GetX() - cameraPosition.GetX()) * worldPixels;
    const float y = static_cast<float>(fixture.height) * .5f - (center.GetY() - cameraPosition.GetY()) * worldPixels;
    const float halfWidth = (text.GetMaxWidth() * .5f + text.GetBackgroundPadding().GetX()) /
        text.GetPixelsPerUnit() * worldPixels;
    const float halfHeight = (rect.GetDesiredSize().height * .5f + text.GetBackgroundPadding().GetY()) /
        text.GetPixelsPerUnit() * worldPixels;
    Require(text.IsEnabled() && (wrapped ? rect.GetDesiredSize().height > text.GetFontSize() * 2 :
            rect.GetDesiredSize().height < text.GetFontSize() * 2),
        "speech capture did not reflect the short/wrapped message height");
    const float screenFontSize = text.GetFontSize() / text.GetPixelsPerUnit() * worldPixels;
    const float uiScale = static_cast<float>(fixture.width) / 1280;
    Require(screenFontSize / uiScale >= 12 && screenFontSize / uiScale <= 14,
        "speech font is not readable at the production camera's wider view");
    std::size_t glyphs = 0;
    std::size_t background = 0;
    const int left = (std::max)(0, static_cast<int>(std::floor(x - halfWidth)));
    const int top = (std::max)(0, static_cast<int>(std::floor(y - halfHeight)));
    const int right = (std::min)(static_cast<int>(shown.width), static_cast<int>(std::ceil(x + halfWidth)));
    const int bottom = (std::min)(static_cast<int>(shown.height), static_cast<int>(std::ceil(y + halfHeight)));
    for (int py = top; py < bottom; ++py)
        for (int px = left; px < right; ++px)
        {
            const auto offset = (static_cast<std::size_t>(py) * shown.width + px) * 4;
            const int r = std::to_integer<int>(shown.pixels[offset]);
            const int g = std::to_integer<int>(shown.pixels[offset + 1]);
            const int b = std::to_integer<int>(shown.pixels[offset + 2]);
            const bool changed = shown.pixels[offset] != baseline.pixels[offset] ||
                shown.pixels[offset + 1] != baseline.pixels[offset + 1] ||
                shown.pixels[offset + 2] != baseline.pixels[offset + 2];
            if (changed && r > 180 && g > 180 && b > 180) ++glyphs;
            if (changed && r < 100 && g < 100 && b < 120) ++background;
        }
    const auto scaleArea = (fixture.width / 1280) * (fixture.height / 720);
    Require(glyphs > 30 * scaleArea && background > 100 * scaleArea,
        "world speech did not render both readable Korean glyphs and its translucent background");
}
}

void RunChatVisualTests()
{
    struct Apartment
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    Require(SUCCEEDED(apartment.result) || apartment.result == RPC_E_CHANGED_MODE,
        "chat capture COM initialization failed");
    const auto directory = (std::filesystem::path(SUMMIT_TEST_CAPTURE_DIRECTORY) /
        (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()))).lexically_normal();
    std::filesystem::create_directories(directory);
    for (const char* name : { "D3D11", "D3D12" })
    {
        const auto* backend = Rendering::GraphicsBackendRegistry::Find(name);
        Require(backend && backend->IsSupported(), "chat visual checks require both D3D11 and D3D12");
        auto device = backend->CreateDevice();
        Require(device && device->Initialize(GameEngine::Platform::NativeSurface{}),
            "chat visual device initialization failed");
        for (const unsigned int scale : { 1u, 2u })
        {
            Fixture fixture(scale);
            const std::string prefix = std::string(name) + "-scale" + std::to_string(scale);
            fixture.chat->SetAvailable(false);
            fixture.Step();
            const auto baseline = Capture(fixture, *device, directory, prefix + "-game");
            fixture.chat->SetAvailable(true);
            fixture.Step();
            const auto emptyPanel = Capture(fixture, *device, directory, prefix + "-empty-chat");
            fixture.chat->AddSystemMessage("서버 채팅에 연결되었습니다.");
            fixture.chat->AddMessage("상여자", "안녕하세요! 함께 정상까지 올라가 봐요.");
            fixture.chat->AddMessage("Player B", "Nice jump! 다음 발판에서 기다릴게요.");
            fixture.chat->AddMessage("심심이심셔",
                "긴 한글 메시지도 패널 너비에 맞춰 자연스럽게 줄바꿈됩니다. "
                "이야기를 읽는 동안에는 위아래로 스크롤해서 지난 채팅을 확인할 수 있어요.");
            fixture.chat->AddMessage("Player E", "준비됐어요. 출발!");
            fixture.Step();
            const auto history = Capture(fixture, *device, directory, prefix + "-history");
            CheckChatPixels(fixture, baseline, history);
            CheckHistoryClipping(fixture, emptyPanel, history);
            auto* player = fixture.scene->FindGameObject("Player");
            auto* speech = Summit::SpeechBubble::ShowMessage(*player, "안녕");
            Require(speech != nullptr, "speech visual fixture could not display its message");
            for (int step = 0; step < 3; ++step) fixture.Step();
            const auto shortSpeech = Capture(fixture, *device, directory, prefix + "-speech-short");
            CheckSpeechPixels(fixture, *speech->GetGameObject(), history, shortSpeech, false);
            Summit::SpeechBubble::ShowMessage(*player,
                "안녕하세요! 말풍선도 캐릭터와 함께 움직여요. 긴 한글 메시지는 머리 위에서 줄바꿈되고 "
                "지난 대화의 전체 내용은 왼쪽 채팅 패널에 그대로 남습니다.");
            for (int step = 0; step < 3; ++step) fixture.Step();
            const auto speechImage = Capture(fixture, *device, directory, prefix + "-speech-bubble-korean");
            CheckSpeechPixels(fixture, *speech->GetGameObject(), history, speechImage, true);
            Summit::SpeechBubble::ShowMessage(*player, "안녕");
            for (int step = 0; step < 3; ++step) fixture.Step();
            const auto smallAgain = Capture(fixture, *device, directory, prefix + "-speech-short-again");
            Require(smallAgain.pixels == shortSpeech.pixels,
                "replacing long speech with the original short text did not restore the compact rendered size");
            speech->GetGameObject()->SetActive(false);
            player->GetComponent<Summit::PlayerController>()->SetSelectedCharacter(Summit::PlayerController::Character::Tdw);
            fixture.Step();
            const auto tdwBaseline = Capture(fixture, *device, directory, prefix + "-tdw-game");
            Summit::SpeechBubble::ShowMessage(*player, "안녕");
            for (int step = 0; step < 3; ++step) fixture.Step();
            const auto tdwSpeech = Capture(fixture, *device, directory, prefix + "-tdw-speech-short");
            CheckSpeechPixels(fixture, *speech->GetGameObject(), tdwBaseline, tdwSpeech, false);
            speech->GetGameObject()->SetActive(false);
            fixture.Enter();
            GameEngine::Platform::InputState typing;
            typing.typedText = "안녕하세요! 저도 함께 갈게요. ";
            typing.compositionText = "가";
            fixture.Step(typing);
            Require(fixture.chat->IsEditing() && fixture.Component<Runtime::InputField>("ChatInput").IsFocused(),
                "editing capture must show an actual focused InputField");
            const auto editing = Capture(fixture, *device, directory, prefix + "-editing-korean");
            CheckChatPixels(fixture, baseline, editing);
            Require(editing.pixels != history.pixels, "the chat composer did not change the rendered frame");
            fixture.chat->SetAvailable(false);
            fixture.scene->FindGameObject("ConnectionCanvas")->SetActive(true);
            fixture.Step();
            const auto profile = Capture(fixture, *device, directory, prefix + "-profile-bgm");
            Require(profile.pixels != editing.pixels, "profile BGM settings did not appear in the rendered frame");

            // Reuse the live scene while resizing: anchors must track the current surface, not startup dimensions.
            fixture.scene->FindGameObject("ConnectionCanvas")->SetActive(false);
            fixture.chat->SetAvailable(true);
            for (const auto size : { std::pair{ 640u, 360u }, std::pair{ 1920u, 1080u } })
            {
                fixture.width = size.first * scale;
                fixture.height = size.second * scale;
                fixture.game.SetRenderSurfaceSize(static_cast<float>(fixture.width), static_cast<float>(fixture.height));
                fixture.game.SetRenderAspectRatio(static_cast<float>(fixture.width) / fixture.height);
                fixture.chat->AddServerNotice(Summit::ChatController::NoticeKind::Announcement, "[공지] 함께 올라가요.");
                fixture.chat->AddServerNotice(Summit::ChatController::NoticeKind::Join, "친구님이 입장했습니다.");
                fixture.Step();
                const auto notices = Capture(fixture, *device, directory,
                    prefix + "-notices-" + std::to_string(size.first));
                CheckNoticePixels(fixture, notices);
            }
        }
    }
    std::cout << "Summit chat visuals passed on D3D11/D3D12 at scale 1/2.\n";
}
