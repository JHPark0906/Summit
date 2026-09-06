#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Assets/Asset.h"
#include "Assets/AssetDatabase.h"
#include "Assets/AudioData.h"
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
#include "Serialization/SceneSerializer.h"
#include "UI/ChatController.h"

namespace
{
namespace Runtime = GameEngine::Runtime;
namespace Platform = GameEngine::Platform;
namespace Assets = GameEngine::Assets;
namespace Serialization = GameEngine::Serialization;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

bool Near(const double first, const double second)
{
    return std::abs(first - second) < 0.00001;
}

// Records the actual voice submitted by AudioSystem without opening an output device.
// A voice is deliberately never reported as finished; assertions concern playback requests.
class RecordingOutput final : public Platform::IAudioOutput
{
public:
    struct Voice
    {
        bool loop = false;
        bool started = false;
        bool destroyed = false;
        float volume = 0;
        std::size_t sampleCount = 0;
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
    };

    bool Initialize() override { return true; }
    std::uint64_t CreateVoice(const Platform::AudioVoiceDescription& description) override
    {
        Require(description.channelCount != 0 && description.sampleRate != 0 && !description.samples.empty(),
            "gameplay AudioSystem submitted an empty voice");
        voices.push_back(Voice{ description.loop, false, false, 0, description.samples.size(),
            description.sampleRate, description.channelCount });
        return static_cast<std::uint64_t>(voices.size());
    }
    void StartVoice(const std::uint64_t id) override { voices.at(static_cast<std::size_t>(id - 1)).started = true; }
    void StopVoice(const std::uint64_t id) override { voices.at(static_cast<std::size_t>(id - 1)).started = false; }
    void SetVoiceVolume(const std::uint64_t id, const float volume) override
    {
        voices.at(static_cast<std::size_t>(id - 1)).volume = volume;
    }
    bool IsVoiceFinished(const std::uint64_t) override { return false; }
    void DestroyVoice(const std::uint64_t id) override { voices.at(static_cast<std::size_t>(id - 1)).destroyed = true; }

    [[nodiscard]] const Voice& Current(const bool looping) const
    {
        const auto found = std::find_if(voices.rbegin(), voices.rend(), [looping](const Voice& voice)
            { return voice.loop == looping && !voice.destroyed; });
        Require(found != voices.rend(), "an expected gameplay audio voice was not created");
        return *found;
    }

    std::vector<Voice> voices;
};

struct Fixture
{
    Platform::DirectoryContentSource content{ std::filesystem::path(SUMMIT_TEST_CONTENT_DIRECTORY) };
    RecordingOutput* output = nullptr;
    Runtime::Game game{ MakeOutput(), nullptr };
    Runtime::Scene* scene = nullptr;

    [[nodiscard]] std::unique_ptr<Platform::IAudioOutput> MakeOutput()
    {
        auto result = std::make_unique<RecordingOutput>();
        output = result.get();
        return result;
    }

    Fixture()
    {
        Require(game.GetAssetDatabase().Refresh(content), "gameplay audio assets could not be registered");
        std::vector<std::byte> bytes;
        Require(content.Read("Scenes/InGame.scene", bytes), "gameplay audio scene could not be read");
        auto loaded = Serialization::SceneSerializer::LoadFromBytes(
            bytes, "Scenes/InGame.scene", game.GetRuntimeContext());
        Require(loaded != nullptr, "gameplay audio scene could not be loaded");
        scene = loaded.get();
        Component<Summit::MultiplayerController>("NetworkManager").SetServerAddress("invalid-ip-for-audio-test");
        Require(game.GetSceneManager().AddScene(std::move(loaded)) != 0, "gameplay audio scene could not be adopted");
        game.SetRenderSurfaceSize(1280, 720);
        game.SetRenderAspectRatio(1280.0f / 720.0f);
        Step();
    }

    template<class T> T& Component(const char* name)
    {
        auto* object = scene->FindGameObject(name);
        auto* component = object ? object->GetComponent<T>() : nullptr;
        Require(component != nullptr, "gameplay audio fixture is missing a component on " + std::string(name));
        return *component;
    }

    void Step(Platform::InputState state = {})
    {
        state.hasFocus = true;
        game.GetInput().BeginFrameWithState(state);
        game.Update(1.0f / 60.0f);
    }

    void Press(const Platform::Key key)
    {
        Platform::InputState state;
        state.SetKey(key, true);
        Step(state);
        Step();
        Step();
    }

    void Click(const char* name)
    {
        const auto rect = Component<Runtime::RectTransform>(name).GetResolvedRect();
        Require(!rect.IsEmpty(), "gameplay audio fixture clicked an unlaid-out control");
        Platform::InputState state;
        state.cursor = { static_cast<int>(rect.GetCenterX()), static_cast<int>(rect.GetCenterY()) };
        Step(state);
        state.SetMouseButton(Platform::MouseButton::Left, true);
        Step(state);
        state.TakeAccumulated();
        state.SetMouseButton(Platform::MouseButton::Left, false);
        Step(state);
        state.TakeAccumulated();
        Step(state);
    }

    void Land(Platform::InputState state = {})
    {
        auto& body = Component<Runtime::Rigidbody2D>("Player");
        for (int frame = 0; frame < 240; ++frame)
        {
            Step(state);
            if (body.IsGrounded()) return;
        }
        throw std::runtime_error("production player did not land on the real scene geometry");
    }
};

[[nodiscard]] std::shared_ptr<const Assets::AudioData> RequireClip(
    Fixture& fixture, const Runtime::AudioSource& source, const std::filesystem::path& expectedPath)
{
    auto& database = fixture.game.GetAssetDatabase();
    const auto* byPath = database.FindAsset(expectedPath);
    Require(source.GetClip().IsGuidReference() && byPath != nullptr &&
        database.FindAsset(source.GetClip()) == byPath && byPath->GetType() == Assets::AssetType::AudioClip,
        "scene audio GUID did not resolve to the intended WAV asset");
    const auto audio = database.LoadAudioClip(source.GetClip());
    Require(audio && audio->IsValid() && std::isfinite(audio->GetDurationSeconds()) &&
        audio->GetDurationSeconds() > 0 && audio->samples.size() % audio->channelCount == 0 &&
        std::any_of(audio->samples.begin(), audio->samples.end(), [](const std::int16_t sample) { return sample != 0; }),
        "production WAV did not decode into non-silent, complete PCM frames");
    return audio;
}

void RealScenePlaysBackgroundAndOnlySuccessfulLocalJumps()
{
    Fixture fixture;
    auto& jump = fixture.Component<Runtime::AudioSource>("Player");
    auto& bgm = fixture.Component<Runtime::AudioSource>("BgmAudio");
    auto& body = fixture.Component<Runtime::Rigidbody2D>("Player");
    const auto jumpData = RequireClip(fixture, jump, "Audios/jumpAudio.wav");
    const auto bgmData = RequireClip(fixture, bgm, "Audios/bgm.wav");
    Require(jumpData->sampleRate == 44100 && jumpData->channelCount == 2 &&
        bgmData->sampleRate == 48000 && bgmData->channelCount == 2,
        "production WAV decoding changed the source channels or sample rate");
    Require(!jump.GetPlayOnAwake() && !jump.IsLooping() && !jump.IsPlaying() && Near(jump.GetVolume(), 0.25),
        "local jump sound started automatically or was configured as a loop");
    // The audible attack must arrive within the first 10 ms.
    const std::size_t attackSamples = (jumpData->sampleRate / 100) * jumpData->channelCount;
    Require(jumpData->samples.size() >= attackSamples && std::any_of(jumpData->samples.begin(),
        jumpData->samples.begin() + attackSamples, [](const std::int16_t sample)
        { return std::abs(static_cast<int>(sample)) >= 3277; }),
        "jump audio still has a delayed attack instead of responding immediately");
    Require(bgm.GetPlayOnAwake() && bgm.IsLooping() && bgm.IsPlaying() && Near(bgm.GetVolume(), 0.25),
        "BGM did not start as a quiet loop when the game scene became active");
    Require(Near(bgm.GetClipDurationSeconds(), bgmData->GetDurationSeconds()), "BGM duration did not match decoded WAV");
    const auto& musicVoice = fixture.output->Current(true);
    Require(musicVoice.started && Near(musicVoice.volume, bgm.GetVolume()) &&
        musicVoice.sampleCount == bgmData->samples.size() && musicVoice.sampleRate == bgmData->sampleRate,
        "BGM loop, audible listener gain, or decoded PCM did not reach the audio output");

    // Even when grounded, the required first profile modal must prevent jump input and sound.
    fixture.Land();
    const auto silentRevision = jump.GetPlaybackRevision();
    fixture.Press(Platform::Key::Space);
    Require(jump.GetPlaybackRevision() == silentRevision && !jump.IsPlaying(),
        "jump audio played through the initial nickname modal");
    fixture.Component<Runtime::InputField>("NicknameInput").SetText("AudioPlayer");
    fixture.Step();
    fixture.Click("NicknameSubmitButton");
    Require(fixture.Component<Summit::MultiplayerController>("NetworkManager").GetConnectionState() ==
        Summit::MultiplayerController::ConnectionState::OfflineSinglePlayer,
        "audio fixture did not use deterministic offline play");
    fixture.Land();

    Platform::InputState heldSpace;
    heldSpace.SetKey(Platform::Key::Space, true);
    fixture.Step(heldSpace);
    const auto firstJumpRevision = jump.GetPlaybackRevision();
    Require(firstJumpRevision == silentRevision + 1 && body.GetVelocity().GetY() > 0 && jump.IsPlaying(),
        "a successful local jump did not play its sound exactly once");
    Require(Near(jump.GetClipDurationSeconds(), jumpData->GetDurationSeconds()),
        "jump sound did not synchronize the actual WAV duration");
    const auto& jumpVoice = fixture.output->Current(false);
    Require(jumpVoice.started && Near(jumpVoice.volume, jump.GetVolume()) &&
        jumpVoice.sampleCount == jumpData->samples.size() && jumpVoice.channels == 2,
        "local jump did not submit its configured one-shot PCM voice");
    heldSpace.TakeAccumulated(); // A held key carries no new press event on later platform frames.
    for (int frame = 0; frame < 4; ++frame) fixture.Step(heldSpace);
    Require(!body.IsGrounded() && jump.GetPlaybackRevision() == firstJumpRevision,
        "holding Space replayed the jump sound while airborne");
    fixture.Step(); // Release, then attempt another key edge in mid-air.
    heldSpace.SetKey(Platform::Key::Space, false);
    heldSpace.SetKey(Platform::Key::Space, true);
    fixture.Step(heldSpace);
    Require(jump.GetPlaybackRevision() == firstJumpRevision, "an invalid airborne jump played sound");
    heldSpace.TakeAccumulated();
    fixture.Land(heldSpace);
    for (int frame = 0; frame < 3; ++frame) fixture.Step(heldSpace);
    Require(jump.GetPlaybackRevision() == firstJumpRevision && body.IsGrounded(),
        "held Space retriggered a jump or its sound when landing");
    fixture.Step();

    fixture.Press(Platform::Key::Enter);
    Require(fixture.Component<Summit::ChatController>("NetworkManager").IsEditing(), "chat did not open in audio fixture");
    fixture.Press(Platform::Key::Space);
    Require(jump.GetPlaybackRevision() == firstJumpRevision && body.IsGrounded(),
        "typing in chat triggered the local jump sound");
    fixture.Press(Platform::Key::Escape); // Cancel chat; the same key must not open the profile modal.
    fixture.Press(Platform::Key::Escape); // Open profile editing.
    fixture.Press(Platform::Key::Space);
    Require(jump.GetPlaybackRevision() == firstJumpRevision && body.IsGrounded(),
        "profile editing triggered the local jump sound");
    fixture.Press(Platform::Key::Escape);
    heldSpace.SetKey(Platform::Key::Space, false);
    heldSpace.SetKey(Platform::Key::Space, true);
    fixture.Step(heldSpace);
    Require(jump.GetPlaybackRevision() == firstJumpRevision + 1 && body.GetVelocity().GetY() > 0,
        "a later grounded jump did not replay audio after UI editing closed");
    Require(bgm.IsPlaying() && fixture.output->Current(true).started,
        "jump or modal/chat input interrupted background music");
}
}

void RunGameplayAudioTests()
{
    RealScenePlaysBackgroundAndOnlySuccessfulLocalJumps();
}
