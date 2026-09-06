#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Assets/AssetDatabase.h"
#include "Gameplay/PlayerController.h"
#include "Network/MultiplayerController.h"
#include "Platform/DirectoryContentSource.h"
#include "Platform/IAudioOutput.h"
#include "Platform/IInput.h"
#include "Platform/ITextMeasure.h"
#include "Runtime/AudioSource.h"
#include "Runtime/Game.h"
#include "Runtime/GameObject.h"
#include "Runtime/Input.h"
#include "Runtime/InputField.h"
#include "Runtime/RectTransform.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/TilemapCollider2D.h"
#include "Runtime/Transform.h"
#include "Serialization/SceneSerializer.h"
#include "UI/ChatController.h"

namespace
{
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;
namespace Serialization = GameEngine::Serialization;
namespace Math = GameEngine::Math;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

bool Near(const Math::Vector3& left, const Math::Vector3& right)
{
    return std::abs(left.GetX() - right.GetX()) < 0.0001f &&
        std::abs(left.GetY() - right.GetY()) < 0.0001f && std::abs(left.GetZ() - right.GetZ()) < 0.0001f;
}

struct Fixture
{
    Platform::DirectoryContentSource content{ std::filesystem::path(SUMMIT_TEST_CONTENT_DIRECTORY) };
    Runtime::Game game{ nullptr, nullptr };
    Runtime::Scene* scene = nullptr;

    Fixture()
    {
        Require(game.GetAssetDatabase().Refresh(content), "respawn assets could not be registered");
        std::vector<std::byte> bytes;
        Require(content.Read("Scenes/InGame.scene", bytes), "respawn scene could not be read");
        auto loaded = Serialization::SceneSerializer::LoadFromBytes(bytes, "Scenes/InGame.scene", game.GetRuntimeContext());
        Require(loaded != nullptr, "respawn scene could not be loaded");
        scene = loaded.get();
        Component<Summit::MultiplayerController>("NetworkManager").SetServerAddress("invalid-ip-for-respawn-test");
        Require(game.GetSceneManager().AddScene(std::move(loaded)) != 0, "respawn scene could not be adopted");
        game.SetRenderSurfaceSize(1280, 720);
        game.SetRenderAspectRatio(1280.0f / 720.0f);
        Step();
    }

    template<class T> T& Component(const char* name)
    {
        auto* object = scene->FindGameObject(name);
        auto* component = object ? object->GetComponent<T>() : nullptr;
        Require(component != nullptr, "respawn fixture is missing a component on " + std::string(name));
        return *component;
    }

    void Step(Platform::InputState input = {}, const float delta = 0)
    {
        input.hasFocus = true;
        game.GetInput().BeginFrameWithState(input);
        game.Update(delta);
    }

    void Press(const Platform::Key key)
    {
        Platform::InputState input;
        input.SetKey(key, true);
        Step(input);
        Step();
        Step();
    }

    void ConfigureProfile()
    {
        Component<Runtime::InputField>("NicknameInput").SetText("RespawnUser");
        Step();
        const auto rect = Component<Runtime::RectTransform>("NicknameSubmitButton").GetResolvedRect();
        Platform::InputState input;
        input.cursor = { static_cast<int>(rect.GetCenterX()), static_cast<int>(rect.GetCenterY()) };
        Step(input);
        input.SetMouseButton(Platform::MouseButton::Left, true);
        Step(input);
        input.TakeAccumulated();
        input.SetMouseButton(Platform::MouseButton::Left, false);
        Step(input);
        input.TakeAccumulated();
        Step(input);
        Require(Component<Summit::MultiplayerController>("NetworkManager").GetNickname() == "RespawnUser",
            "respawn fixture did not apply its offline nickname");
    }

    void Land()
    {
        auto& body = Component<Runtime::Rigidbody2D>("Player");
        for (int frame = 0; frame < 180; ++frame)
        {
            Step({}, 1.0f / 60.0f);
            if (body.IsGrounded()) return;
        }
        throw std::runtime_error("respawn player did not land on the production map");
    }
};

void FallingReturnsToInitialPositionWithoutReplayingJump()
{
    Fixture fixture;
    auto& player = fixture.Component<Summit::PlayerController>("Player");
    auto& transform = player.GetGameObject()->GetTransform();
    auto& body = fixture.Component<Runtime::Rigidbody2D>("Player");
    auto& audio = fixture.Component<Runtime::AudioSource>("Player");
    const auto start = transform.GetWorldPosition();
    const auto audioRevision = audio.GetPlaybackRevision();
    const auto mapBounds = fixture.Component<Runtime::TilemapCollider2D>("TileMap").GetWorldBounds();
    Require(player.GetRespawnY() == -10 && player.GetRespawnY() < mapBounds.min.GetY(),
        "the default respawn threshold is not below the production terrain");
    fixture.Land();
    Require(body.IsGrounded(), "the stale-ground respawn case must start with a real contact");

    // No fixed step runs here. The respawn itself must clear old contacts and velocities,
    // including when the required nickname modal currently blocks gameplay input.
    Require(transform.SetWorldPosition({ 100, player.GetRespawnY() - 1, 2 }), "could not place player below the map");
    body.SetVelocity({ 50, -90 });
    Platform::InputState jumping;
    jumping.SetKey(Platform::Key::Space, true);
    jumping.SetKey(Platform::Key::D, true);
    fixture.Step(jumping);
    Require(Near(transform.GetWorldPosition(), start) && body.GetVelocity().GetX() == 0 &&
        body.GetVelocity().GetY() == 0 && !body.IsGrounded() &&
        body.GetContacts() == Runtime::Rigidbody2DContact::None,
        "respawn retained fall velocity/ground contact or failed through the profile modal");
    Require(audio.GetPlaybackRevision() == audioRevision, "respawn played the jump sound");
    fixture.Step();
    fixture.ConfigureProfile();
    player.SetSelectedCharacter(Summit::PlayerController::Character::CharacterE);

    // Being exactly on the boundary is still inside; crossing below it triggers the reset.
    Require(transform.SetWorldPosition({ 100, player.GetRespawnY(), 2 }), "could not place player on the boundary");
    fixture.Step();
    Require(transform.GetWorldPosition().GetX() == 100, "respawn triggered before crossing the configured boundary");
    Require(transform.SetWorldPosition({ 100, player.GetRespawnY() - 0.01f, 2 }), "could not cross the boundary");
    fixture.Step(jumping);
    Require(Near(transform.GetWorldPosition(), start) && body.GetVelocity().GetY() == 0 &&
        audio.GetPlaybackRevision() == audioRevision,
        "active gameplay input jumped or replayed audio in the respawn frame");
    fixture.Step();

    fixture.Press(Platform::Key::Enter);
    Require(fixture.Component<Summit::ChatController>("NetworkManager").IsEditing(), "chat did not open in respawn test");
    body.SetEnabled(false);
    Require(transform.SetWorldPosition({ -100, -30, 0 }), "could not place paused player below the map");
    body.SetVelocity({ -20, -30 });
    fixture.Step();
    Require(Near(transform.GetWorldPosition(), start) && body.GetVelocity().GetY() == 0 && !body.IsGrounded(),
        "chat editing or disabled physics prevented the safety respawn");
    body.SetEnabled(true);
    fixture.Press(Platform::Key::Escape);

    // A real fall outside the tilemap is detected on the next Update, before that frame's
    // physics. The new fixed step may add gravity, but must not reuse the former fall speed.
    Require(transform.SetWorldPosition({ mapBounds.max.GetX() + 10, start.GetY(), start.GetZ() }),
        "could not place player outside the map edge");
    body.SetVelocity({ 0, -2 });
    bool returned = false;
    for (int frame = 0; frame < 240 && !returned; ++frame)
    {
        fixture.Step({}, 1.0f / 60.0f);
        returned = std::abs(transform.GetWorldPosition().GetX() - start.GetX()) < 0.001f;
    }
    Require(returned && transform.GetWorldPosition().GetY() > start.GetY() - 0.01f &&
        body.GetVelocity().GetY() > -0.2f && body.GetVelocity().GetY() <= 0,
        "a simulated fall did not return safely to the original start point");
    Require(player.GetSelectedCharacter() == Summit::PlayerController::Character::CharacterE &&
        fixture.Component<Summit::MultiplayerController>("NetworkManager").GetNickname() == "RespawnUser" &&
        fixture.Component<Runtime::TextRenderer>("NicknameLabel").GetText() == "RespawnUser" &&
        audio.GetPlaybackRevision() == audioRevision,
        "respawn replaced the committed character/nickname or replayed jump audio");
}

void ThresholdRoundTripsAndStartUsesWorldCoordinates()
{
    Runtime::Game game(nullptr, nullptr);
    auto scene = std::make_unique<Runtime::Scene>(game.GetRuntimeContext(), "ParentedRespawn");
    auto* parent = scene->CreateGameObject("Parent");
    parent->GetTransform().SetPosition({ 10, 20, 0 });
    parent->GetTransform().SetScale({ 2, 2, 1 });
    auto* object = scene->CreateGameObject("Player");
    Require(object->GetTransform().SetParent(&parent->GetTransform()), "parented respawn fixture cannot set parent");
    object->GetTransform().SetPosition({ 1, 2, 3 });
    auto* body = object->AddComponent<Runtime::Rigidbody2D>();
    body->SetGravityScale(0);
    auto* player = object->AddComponent<Summit::PlayerController>();
    player->SetRespawnY(-25);
    const auto serialized = Serialization::SceneSerializer::SaveToText(*scene);
    auto restored = Serialization::SceneSerializer::LoadFromBytes(
        std::as_bytes(std::span(serialized.data(), serialized.size())), "respawn-test.scene", game.GetRuntimeContext());
    Require(restored && restored->FindGameObject("Player") &&
        restored->FindGameObject("Player")->GetComponent<Summit::PlayerController>()->GetRespawnY() == -25,
        "the inspector respawn threshold did not survive scene serialization");
    for (const float invalid : { (std::numeric_limits<float>::infinity)(),
        -(std::numeric_limits<float>::infinity)(), (std::numeric_limits<float>::quiet_NaN)() })
    {
        player->SetRespawnY(invalid);
        Require(player->GetRespawnY() == -10, "invalid respawn threshold did not restore the finite default");
    }
    Require(game.GetSceneManager().AddScene(std::move(scene)) != 0, "parented respawn scene could not be adopted");
    game.Update(0);
    const auto start = object->GetTransform().GetWorldPosition();
    parent->GetTransform().SetPosition({ 30, 40, 0 });
    Require(object->GetTransform().SetWorldPosition({ 70, -11, 0 }), "could not move parented player below threshold");
    body->SetVelocity({ 12, -50 });
    game.Update(0);
    Require(Near(object->GetTransform().GetWorldPosition(), start) && body->GetVelocity().GetY() == 0,
        "respawn used the changed parent/local coordinates instead of the initial world point");
}
}

void RunRespawnTests()
{
    FallingReturnsToInitialPositionWithoutReplayingJump();
    ThresholdRoundTripsAndStartUsesWorldCoordinates();
}
