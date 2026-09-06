#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "UI/ChatController.h"
#include "Network/MultiplayerController.h"
#include "Assets/AssetDatabase.h"
#include "Platform/DirectoryContentSource.h"
#include "Platform/IAudioOutput.h"
#include "Platform/PlatformServices.h"
#include "Rendering/CachedTextMeasure.h"
#include "Rendering/TextRasterizationCache.h"
#include "Runtime/Canvas.h"
#include "Runtime/Game.h"
#include "Runtime/Input.h"
#include "Runtime/InputField.h"
#include "Runtime/RectTransform.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Serialization/SceneSerializer.h"

namespace ChatTestSupport
{
inline void Require(const bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Fixture
{
    GameEngine::Platform::DirectoryContentSource content{ SUMMIT_TEST_CONTENT_DIRECTORY };
    std::shared_ptr<GameEngine::Rendering::TextRasterizationCache> cache =
        std::make_shared<GameEngine::Rendering::TextRasterizationCache>(
            GameEngine::Platform::PlatformServices::CreateTextRasterizer());
    GameEngine::Runtime::Game game{ nullptr,
        std::make_unique<GameEngine::Rendering::CachedTextMeasure>(cache) };
    GameEngine::Runtime::Scene* scene = nullptr;
    Summit::ChatController* chat = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;

    explicit Fixture(const unsigned int scale) : width(1280 * scale), height(720 * scale)
    {
        std::vector<std::byte> bytes;
        Require(content.Read("Fonts/NanumSquareNeoOTF-Bd.otf", bytes) && cache->RegisterFont("Summit UI", bytes),
            "chat tests require Summit's actual Korean font");
        Require(game.GetAssetDatabase().Refresh(content), "chat production assets could not be registered");
        Require(content.Read("Scenes/InGame.scene", bytes), "chat production scene could not be read");
        auto value = GameEngine::Serialization::SceneSerializer::LoadFromBytes(bytes,
            "Scenes/InGame.scene", game.GetRuntimeContext());
        Require(value != nullptr, "chat production scene could not be loaded");
        scene = value.get();
        auto* manager = scene->FindGameObject("NetworkManager");
        Require(manager != nullptr, "chat scene has no manager");
        manager->GetComponent<Summit::MultiplayerController>()->SetEnabled(false);
        chat = manager->GetComponent<Summit::ChatController>();
        Require(chat != nullptr, "chat controller is absent from the production scene");
        scene->FindGameObject("ConnectionCanvas")->SetActive(false);
        scene->FindGameObject("Player")->GetComponent<GameEngine::Runtime::Rigidbody2D>()->SetEnabled(false);
        for (const auto& [id, object] : scene->GetGameObjects())
        {
            static_cast<void>(id);
            if (auto* canvas = object->GetComponent<GameEngine::Runtime::Canvas>())
                canvas->SetScaleFactor(static_cast<float>(scale));
        }
        Require(game.GetSceneManager().AddScene(std::move(value)) != 0, "chat scene could not be adopted");
        game.SetRenderSurfaceSize(static_cast<float>(width), static_cast<float>(height));
        game.SetRenderAspectRatio(1280.0f / 720.0f);
        chat->SetAvailable(true);
        Step();
    }

    template<class T> T& Component(const char* name)
    {
        auto* object = scene->FindGameObject(name);
        auto* component = object ? object->GetComponent<T>() : nullptr;
        Require(component != nullptr, "required production chat UI component is missing");
        return *component;
    }

    void Step(GameEngine::Platform::InputState input = {})
    {
        input.hasFocus = true;
        game.GetInput().BeginFrameWithState(input);
        chat->TickInput();
        game.Update(0);
    }

    void Enter()
    {
        GameEngine::Platform::InputState state;
        state.SetKey(GameEngine::Platform::Key::Enter, true);
        Step(state);
        Step();
    }

    void Click(const char* name)
    {
        const auto rect = Component<GameEngine::Runtime::RectTransform>(name).GetResolvedRect();
        GameEngine::Platform::InputState state;
        state.cursor = { static_cast<int>(rect.GetCenterX()), static_cast<int>(rect.GetCenterY()) };
        state.SetMouseButton(GameEngine::Platform::MouseButton::Left, true);
        Step(state);
        state.TakeAccumulated();
        state.SetMouseButton(GameEngine::Platform::MouseButton::Left, false);
        Step(state);
        state.TakeAccumulated();
        Step(state);
    }
};
}
