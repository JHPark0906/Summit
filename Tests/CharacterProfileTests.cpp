#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Gameplay/CharacterProfile.h"
#include "UI/ChatController.h"
#include "Gameplay/PlayerController.h"
#include "Network/MultiplayerController.h"
#include "Assets/AssetReference.h"
#include "Assets/AssetDatabase.h"
#include "Core/Json.h"
#include "Platform/DirectoryContentSource.h"
#include "Platform/IAudioOutput.h"
#include "Platform/IInput.h"
#include "Platform/ITextMeasure.h"
#include "Runtime/BoxCollider2D.h"
#include "Runtime/AudioSource.h"
#include "Runtime/Button.h"
#include "Runtime/Canvas.h"
#include "Runtime/Game.h"
#include "Runtime/GameObject.h"
#include "Runtime/Input.h"
#include "Runtime/InputField.h"
#include "Runtime/RectTransform.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Runtime/SpriteAnimator.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/TextRenderer.h"
#include "Runtime/Transform.h"
#include "Serialization/RuntimeComponentFactories.h"
#include "Serialization/SceneSerializer.h"

void RunChatUiTests();
void RunChatNetworkTests();
void RunSpeechBubbleTests();
void RunRespawnTests();
void RunOneWayPlatformTests();
void RunGameplayAudioTests();
void RunChatVisualTests();
void RunCameraBackgroundTests();

namespace
{
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;
namespace Serialization = GameEngine::Serialization;
using Summit::PlayerController;
using Character = PlayerController::Character;
using Animation = PlayerController::AnimationState;

constexpr std::array<Character, 6> Characters{ Character::CharacterA, Character::CharacterB,
    Character::CharacterC, Character::CharacterD, Character::CharacterE, Character::Tdw };
constexpr std::array<std::string_view, 6> SerializedCharacters{
    "characterA", "characterB", "characterC", "characterD", "characterE", "tdw" };
constexpr std::array<std::string_view, 5> CharacterButtons{
    "CharacterAButton", "CharacterBButton", "CharacterCButton", "CharacterDButton", "CharacterEButton" };

void Require(const bool condition, const std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

bool Near(const float left, const float right)
{
    return std::abs(left - right) < 0.001f;
}

void ProfileRulesKeepSelectionAndRequireExactNames()
{
    Require(Summit::SelectableCharacterCount == 5 && Summit::CharacterCount == 6,
        "five regular characters and one nickname character must remain distinct");
    for (unsigned int index = 0; index < Characters.size(); ++index)
        Require(static_cast<unsigned int>(Characters[index]) == index, "wire character IDs changed");

    for (const std::string_view name : { "상여자", "심심이심셔" })
    {
        for (unsigned int selected = 0; selected <= 5; ++selected)
            Require(Summit::ResolveProfileCharacter(name, selected) == 5,
                "an exact special nickname must force tdw regardless of prior selection");
        Require(Summit::IsProfileCharacterValid(name, 5), "tdw approval should match the exact name");
        for (unsigned int character = 0; character < 5; ++character)
            Require(!Summit::IsProfileCharacterValid(name, character),
                "an ordinary character cannot be approved for a special nickname");
    }

    const std::array<std::string_view, 9> ordinaryNames{ "Player", "플레이어", "", "상여",
        "상여자 ", " 상여자", "심심이심셔!", "심심이심셔 ", "심심이심" };
    for (const auto name : ordinaryNames)
    {
        Require(!Summit::UsesEasterEggCharacter(name), "nickname matching must not trim or match prefixes");
        for (unsigned int selected = 0; selected < 5; ++selected)
        {
            Require(Summit::ResolveProfileCharacter(name, selected) == selected,
                "ordinary names must preserve all A through E selections");
            Require(Summit::IsProfileCharacterValid(name, selected), "a regular character was rejected");
        }
        for (const unsigned int invalid : { 5u, 6u, (std::numeric_limits<unsigned int>::max)() })
        {
            Require(Summit::ResolveProfileCharacter(name, invalid) == 0,
                "leaving tdw with an invalid regular selection must fall back to A");
            Require(!Summit::IsProfileCharacterValid(name, invalid), "invalid approved character was accepted");
        }
    }
    Require(!Summit::IsProfileCharacterValid("상여자", 6), "an out-of-range special approval was accepted");
}

struct Fixture
{
    Runtime::Game game{ nullptr, nullptr };
    Runtime::Scene* scene = nullptr;

    void Adopt(std::unique_ptr<Runtime::Scene> value)
    {
        scene = value.get();
        // These tests execute components and layout, without importing textures or opening a GPU.
        Require(game.GetSceneManager().AddScene(std::move(value)) != 0, "test scene could not be adopted");
    }

    void Step(Platform::InputState state = {}, const float delta = 0.0f)
    {
        state.hasFocus = true;
        game.GetInput().BeginFrameWithState(state);
        game.Update(delta);
    }

    Runtime::GameObject& Object(const std::string_view name)
    {
        auto* object = scene->FindGameObject(std::string(name));
        Require(object != nullptr, "a required game object is missing: " + std::string(name));
        return *object;
    }

    template <typename T> T& Component(const std::string_view name)
    {
        auto* component = Object(name).GetComponent<T>();
        Require(component != nullptr, "a required component is missing: " + std::string(name));
        return *component;
    }

    void Click(const std::string_view name)
    {
        const auto rect = Component<Runtime::RectTransform>(name).GetResolvedRect();
        Require(!rect.IsEmpty() && Object(name).IsActiveInHierarchy(), "click target is not laid out or active");
        Platform::InputState state;
        state.cursor = { static_cast<int>(rect.GetCenterX()), static_cast<int>(rect.GetCenterY()) };
        Step(state);
        state.SetMouseButton(Platform::MouseButton::Left, true);
        Step(state);
        state.TakeAccumulated();
        state.SetMouseButton(Platform::MouseButton::Left, false);
        Step(state);
        state.TakeAccumulated();
        Step(state); // MonoBehaviours consume the click produced by the preceding UI pass.
    }

    void Escape()
    {
        Platform::InputState state;
        state.SetKey(Platform::Key::Escape, true);
        Step(state);
        Step();
    }
};

void CharacterPropertiesSurviveSceneSerialization()
{
    Fixture fixture;
    auto scene = std::make_unique<Runtime::Scene>(fixture.game.GetRuntimeContext(), "CharacterProperties");
    auto* player = scene->CreateGameObject("Player");
    auto* controller = player->AddComponent<PlayerController>();
    for (unsigned int index = 0; index < Characters.size(); ++index)
    {
        for (const auto state : { Animation::Idle, Animation::Walk, Animation::Jump })
        {
            controller->SetAtlas(Characters[index], state, GameEngine::Assets::AssetReference(
                std::filesystem::path("test/character" + std::to_string(index) + "-" +
                    std::to_string(static_cast<unsigned int>(state)) + ".png")));
        }
    }
    fixture.Adopt(std::move(scene));
    for (unsigned int index = 0; index < Characters.size(); ++index)
    {
        controller->SetSelectedCharacter(Characters[index]);
        const auto componentJson = Serialization::SceneSerializer::SaveComponentToJson(*controller);
        Require(componentJson.has_value(), "PlayerController lost its serialization factory");
        const auto* selected = componentJson->Find("selectedCharacter");
        Require(selected && selected->IsString() && selected->AsString() == SerializedCharacters[index],
            "the serialized character name does not match its wire ID");
        const std::string serialized = Serialization::SceneSerializer::SaveToText(*fixture.scene);
        auto restoredScene = Serialization::SceneSerializer::LoadFromBytes(
            std::as_bytes(std::span(serialized.data(), serialized.size())), "character-test.scene",
            fixture.game.GetRuntimeContext());
        Require(restoredScene != nullptr, "the saved character scene cannot be read");
        const auto* restoredPlayer = restoredScene->FindGameObject("Player");
        Require(restoredPlayer != nullptr, "the player object was lost on scene reload");
        const auto* restored = restoredPlayer->GetComponent<PlayerController>();
        Require(restored && restored->GetSelectedCharacter() == Characters[index], "character changed on scene reload");
        for (const auto character : Characters)
            for (const auto state : { Animation::Idle, Animation::Walk, Animation::Jump })
                Require(restored->GetAtlas(character, state) == controller->GetAtlas(character, state),
                    "a character animation atlas was lost on scene reload");
    }
    controller->SetSelectedCharacter(static_cast<Character>(255));
    Require(controller->GetSelectedCharacter() == Character::CharacterA, "invalid character enum was not sanitized");
}

void TdwPingPongResetsWhenMovementOrCharacterChanges()
{
    Fixture fixture;
    auto scene = std::make_unique<Runtime::Scene>(fixture.game.GetRuntimeContext(), "AnimationTransitions");
    auto* floor = scene->CreateGameObject("Floor");
    floor->GetTransform().SetPosition({ 0, -0.5f, 0 });
    floor->AddComponent<Runtime::BoxCollider2D>()->SetSize({ 100, 1 });
    auto* player = scene->CreateGameObject("Player");
    player->GetTransform().SetPosition({ 0, 1.3f, 0 });
    auto* collider = player->AddComponent<Runtime::BoxCollider2D>();
    collider->SetSize({ 1.6f, 2.56f });
    auto* body = player->AddComponent<Runtime::Rigidbody2D>();
    auto* controller = player->AddComponent<PlayerController>();
    auto* visual = scene->CreateGameObject("Visual");
    Require(visual->GetTransform().SetParent(&player->GetTransform()), "visual cannot be parented");
    auto* sprite = visual->AddComponent<Runtime::SpriteRenderer>();
    auto* animator = visual->AddComponent<Runtime::SpriteAnimator>();
    for (const auto character : Characters)
        for (const auto state : { Animation::Idle, Animation::Walk, Animation::Jump })
            controller->SetAtlas(character, state, GameEngine::Assets::AssetReference(
                std::filesystem::path("test/" + std::to_string(static_cast<unsigned int>(character)) +
                    "-" + std::to_string(static_cast<unsigned int>(state)) + ".png")));
    controller->SetSelectedCharacter(Character::Tdw);
    controller->SetGameplayInputEnabled(true);
    fixture.Adopt(std::move(scene));
    for (int frame = 0; frame < 30; ++frame) fixture.Step({}, 1.0f / 60.0f);
    Require(body->IsGrounded(), "test player did not land on its real physics floor");
    Require(animator->IsPingPong() && animator->IsLooping(), "tdw idle must loop in ping-pong mode");
    Require(sprite->GetSprite() == controller->GetAtlas(Character::Tdw, Animation::Idle), "tdw idle atlas was not selected");
    Require(!controller->IsFacingLeft() && sprite->IsFlippedX(), "tdw's left-facing source was not corrected for logical right");
    const float feetY = collider->GetOffset().GetY() - collider->GetSize().GetY() * 0.5f;
    for (const auto state : { Animation::Idle, Animation::Walk, Animation::Jump })
        Require(Near(controller->GetVisualOffsetY(Character::Tdw, state), feetY + 50.0f * 2.0f / 100.0f),
            "tdw feet were not aligned using the doubled source distance");

    Platform::InputState right;
    right.SetKey(Platform::Key::D, true);
    fixture.Step(right, 1.0f / 60.0f);
    Require(body->GetVelocity().GetX() > 0 && !animator->IsPingPong(), "tdw walk retained idle ping-pong");
    Require(sprite->GetSprite() == controller->GetAtlas(Character::Tdw, Animation::Walk), "tdw walk atlas was not selected");
    fixture.Step({}, 1.0f / 60.0f);
    Require(animator->IsPingPong(), "stopping did not restore tdw idle ping-pong");

    Platform::InputState left;
    left.SetKey(Platform::Key::A, true);
    fixture.Step(left, 1.0f / 60.0f);
    Require(body->GetVelocity().GetX() < 0 && controller->IsFacingLeft() && !sprite->IsFlippedX(),
        "tdw logical left did not retain its unflipped source orientation");
    fixture.Step({}, 1.0f / 60.0f);

    for (const auto character : Characters)
    {
        controller->SetSelectedCharacter(character);
        fixture.Step({}, 1.0f / 60.0f);
        Require(animator->IsPingPong() == (character == Character::Tdw), "idle mode leaked across character switches");
        Require(Near(collider->GetSize().GetX(), 1.6f) && Near(collider->GetSize().GetY(), 2.56f),
            "a character switch changed the shared collider dimensions");
        const float expectedScale = character == Character::Tdw ? 2.0f : 1.0f;
        Require(Near(PlayerController::GetVisualScale(character), expectedScale) &&
                Near(visual->GetTransform().GetScale().GetX(), expectedScale) &&
                Near(visual->GetTransform().GetScale().GetY(), expectedScale), "a character switch used the wrong visual scale");
        Require(PlayerController::IsSpriteFlipped(character, false) == (character == Character::Tdw) &&
                PlayerController::IsSpriteFlipped(character, true) == (character != Character::Tdw),
            "source-facing correction changed the logical left/right convention");
        Require(controller->IsFacingLeft() && sprite->IsFlippedX() == (character != Character::Tdw),
            "switching character lost the retained logical facing direction");
    }
    controller->SetGameplayInputEnabled(false);
    fixture.Step(right, 1.0f / 60.0f);
    Require(Near(body->GetVelocity().GetX(), 0), "profile editing must suppress horizontal movement");
}

void OfflineProfileUiPreservesCommittedSelection(const float scale)
{
    Fixture fixture;
    const Platform::DirectoryContentSource content{ std::filesystem::path(SUMMIT_TEST_CONTENT_DIRECTORY) };
    Require(fixture.game.GetAssetDatabase().Refresh(content), "in-game profile test assets could not be registered");
    std::vector<std::byte> bytes;
    Require(content.Read("Scenes/InGame.scene", bytes), "the production in-game scene could not be read");
    auto scene = Serialization::SceneSerializer::LoadFromBytes(bytes, "Scenes/InGame.scene", fixture.game.GetRuntimeContext());
    Require(scene != nullptr, "the production in-game scene could not be loaded");
    for (const auto& [id, object] : scene->GetGameObjects())
    {
        (void)id;
        if (auto* canvas = object->GetComponent<Runtime::Canvas>()) canvas->SetScaleFactor(scale);
    }
    fixture.game.SetRenderSurfaceSize(1280 * scale, 720 * scale);
    fixture.Adopt(std::move(scene));
    fixture.game.SetRenderAspectRatio(1280.0f / 720.0f);
    auto& network = fixture.Component<Summit::MultiplayerController>("NetworkManager");
    network.SetServerAddress("invalid-ip-for-offline-test"); // Deterministic local fallback; no remote connection.
    auto& player = fixture.Component<PlayerController>("Player");
    auto& input = fixture.Component<Runtime::InputField>("NicknameInput");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    Require(network.GetNickname().empty() && fixture.Object("ProfilePanel").IsActive(),
        "single-player started without a nickname");

    const auto panel = fixture.Component<Runtime::RectTransform>("ProfilePanel").GetResolvedRect();
    auto& bgm = fixture.Component<Runtime::AudioSource>("BgmAudio");
    const auto playbackRevision = bgm.GetPlaybackRevision();
    Require(bgm.IsPlaying() && bgm.IsLooping() && Near(bgm.GetVolume(), 0.25f),
        "BGM must start looping before the initial profile is complete");
    fixture.Click("BgmMuteButton");
    Require(Near(bgm.GetVolume(), 0) && bgm.IsPlaying() && bgm.GetPlaybackRevision() == playbackRevision,
        "muting BGM must silence it without stopping or restarting its loop");
    fixture.Click("BgmMuteButton");
    Require(Near(bgm.GetVolume(), 0.25f), "unmuting BGM must restore its previous volume");
    fixture.Click("BgmVolumeUpButton");
    Require(Near(bgm.GetVolume(), 0.35f), "BGM volume increment must apply immediately");
    fixture.Click("BgmVolumeDownButton");
    Require(Near(bgm.GetVolume(), 0.25f), "BGM volume decrement must apply immediately");
    for (const auto name : { "BgmVolumeDownButton", "BgmVolumeText", "BgmVolumeUpButton", "BgmMuteButton" })
    {
        const auto rect = fixture.Component<Runtime::RectTransform>(name).GetResolvedRect();
        Require(!rect.IsEmpty() && rect.x >= panel.x && rect.GetRight() <= panel.GetRight() &&
            rect.y >= panel.y && rect.GetBottom() <= panel.GetBottom(),
            "BGM controls escape the profile panel at this UI scale");
    }
    for (std::size_t index = 0; index < CharacterButtons.size(); ++index)
    {
        const auto rect = fixture.Component<Runtime::RectTransform>(CharacterButtons[index]).GetResolvedRect();
        Require(!rect.IsEmpty() && rect.x >= panel.x && rect.GetRight() <= panel.GetRight() &&
                rect.y >= panel.y && rect.GetBottom() <= panel.GetBottom(), "character option escapes its panel at this UI scale");
        for (std::size_t previous = 0; previous < index; ++previous)
            Require(rect.IntersectedWith(fixture.Component<Runtime::RectTransform>(CharacterButtons[previous]).GetResolvedRect()).IsEmpty(),
                "character buttons overlap at this UI scale");
    }
    input.SetText("RegularPlayer");
    fixture.Step();
    fixture.Click("CharacterEButton");
    fixture.Click("NicknameSubmitButton");
    Require(network.GetNickname() == "RegularPlayer" && player.GetSelectedCharacter() == Character::CharacterE,
        "initial profile did not apply the selected regular character");
    Require(network.GetConnectionState() == Summit::MultiplayerController::ConnectionState::OfflineSinglePlayer,
        "the fixture did not stay offline");

    fixture.Escape();
    input.SetText("상여자");
    fixture.Step();
    for (const auto name : CharacterButtons)
        Require(!fixture.Component<Runtime::Button>(name).IsInteractable(), "special nickname still permits regular-character clicks");
    fixture.Click("NicknameSubmitButton");
    Require(network.GetNickname() == "상여자" && player.GetSelectedCharacter() == Character::Tdw,
        "an offline special nickname did not force tdw");
    Require(fixture.Component<Runtime::TextRenderer>("NicknameLabel").GetText() == "상여자", "nameplate did not track the applied profile");

    fixture.Escape();
    input.SetText("UncommittedPlayer");
    fixture.Step();
    fixture.Click("CharacterCButton");
    fixture.Click("ProfileCancelButton");
    Require(network.GetNickname() == "상여자" && player.GetSelectedCharacter() == Character::Tdw,
        "cancel changed the committed nickname or tdw character");
    fixture.Escape();
    Require(input.GetText() == "상여자", "reopening the editor retained a cancelled nickname");
    input.SetText("RegularAgain");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    Require(player.GetSelectedCharacter() == Character::CharacterE, "leaving tdw did not restore the committed regular selection");

    fixture.Escape();
    input.SetText("심심이심셔");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    Require(player.GetSelectedCharacter() == Character::Tdw, "the second exact nickname did not force tdw");
    fixture.Escape();
    input.SetText("심심이심셔 ");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    Require(player.GetSelectedCharacter() == Character::CharacterE, "nickname matching incorrectly trimmed trailing whitespace");

    auto& chat = fixture.Component<Summit::ChatController>("NetworkManager");
    auto& chatInput = fixture.Component<Runtime::InputField>("ChatInput");
    auto& body = fixture.Component<Runtime::Rigidbody2D>("Player");
    const auto messagesBefore = chat.GetMessageCount();
    Platform::InputState typing;
    typing.SetKey(Platform::Key::Enter, true);
    typing.SetKey(Platform::Key::D, true);
    typing.SetKey(Platform::Key::Space, true);
    fixture.Step(typing);
    Require(chat.IsEditing() && chatInput.IsFocused(), "Enter did not open and focus chat");
    Require(Near(body.GetVelocity().GetX(), 0), "opening chat leaked the simultaneous movement key");
    fixture.Step();
    typing = {};
    typing.typedText = "한글로 대화해요";
    typing.SetKey(Platform::Key::A, true);
    fixture.Step(typing);
    Require(chatInput.GetText() == "한글로 대화해요", "chat did not receive committed Korean input");
    Require(Near(body.GetVelocity().GetX(), 0), "typing A in chat moved the player");
    fixture.Step();
    typing = {};
    typing.SetKey(Platform::Key::Enter, true);
    fixture.Step(typing);
    fixture.Step();
    fixture.Step();
    Require(chat.GetMessageCount() > messagesBefore, "offline chat submission did not enter local history");
    Require(!chat.IsEditing(), "submitting chat did not return control to the game");
    Require(!fixture.Object("ProfilePanel").IsActiveInHierarchy(), "sending chat opened the profile modal");

    fixture.Step(typing);
    fixture.Step();
    Require(chat.IsEditing(), "chat could not reopen after sending");
    chatInput.SetText("취소할 메시지");
    const auto beforeCancel = chat.GetMessageCount();
    fixture.Escape();
    Require(!chat.IsEditing() && !fixture.Object("ProfilePanel").IsActiveInHierarchy(),
        "Escape in chat must cancel chat without opening the profile modal");
    Require(chat.GetMessageCount() == beforeCancel, "cancelled chat was sent");
    fixture.Escape();
    Require(fixture.Object("ProfilePanel").IsActiveInHierarchy(), "Escape outside chat must still open the profile modal");
    fixture.Escape();

    Platform::InputState movement;
    movement.SetKey(Platform::Key::D, true);
    fixture.Step(movement);
    fixture.Step(movement);
    Require(body.GetVelocity().GetX() > 0, "movement did not resume after chat/profile closed");
    fixture.Step();
    const auto openRect = fixture.Component<Runtime::RectTransform>("ChatOpenButton").GetResolvedRect();
    Platform::InputState openClick;
    openClick.cursor = { static_cast<int>(openRect.GetCenterX()), static_cast<int>(openRect.GetCenterY()) };
    openClick.SetKey(Platform::Key::D, true);
    openClick.SetMouseButton(Platform::MouseButton::Left, true);
    fixture.Step(openClick);
    openClick.TakeAccumulated();
    fixture.Step(openClick);
    fixture.Step(openClick);
    openClick.SetMouseButton(Platform::MouseButton::Left, false);
    fixture.Step(openClick);
    Require(Near(body.GetVelocity().GetX(), 0), "releasing the chat open button leaked movement before UI Update");
    openClick.TakeAccumulated();
    fixture.Step(openClick);
    Require(chat.IsEditing(), "releasing the chat open button did not open chat");
    fixture.Escape();
    Platform::InputState click;
    click.cursor = { static_cast<int>(1000 * scale), static_cast<int>(360 * scale) };
    click.SetMouseButton(Platform::MouseButton::Left, true);
    fixture.Step(click);
    fixture.Step();
    Require(fixture.scene->FindGameObject("CatProjectile") == nullptr,
        "the removed cat feature still creates projectiles on a world click");

}
}

int main(const int argc, char** argv)
{
    try
    {
        static_cast<void>(Serialization::RegisterRuntimeComponentFactories());
        static_cast<void>(Serialization::RegisterComponentType(PlayerController::StaticType()));
        static_cast<void>(Serialization::RegisterComponentType(Summit::MultiplayerController::StaticType()));
        static_cast<void>(Serialization::RegisterComponentType(Summit::ChatController::StaticType()));
        if (argc == 2 && std::string_view(argv[1]) == "--chat-visual")
        {
            RunChatVisualTests();
            return 0;
        }
        RunChatUiTests();
        RunChatNetworkTests();
        RunSpeechBubbleTests();
        RunRespawnTests();
        RunOneWayPlatformTests();
        RunGameplayAudioTests();
        RunCameraBackgroundTests();
        ProfileRulesKeepSelectionAndRequireExactNames();
        CharacterPropertiesSurviveSceneSerialization();
        TdwPingPongResetsWhenMovementOrCharacterChanges();
        OfflineProfileUiPreservesCommittedSelection(1.0f);
        OfflineProfileUiPreservesCommittedSelection(2.0f);
        std::cout << "Summit character/profile tests passed (rules, serialization, animation, offline UI at 1x/2x).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Summit character/profile test failed: " << error.what() << '\n';
        return 1;
    }
}
