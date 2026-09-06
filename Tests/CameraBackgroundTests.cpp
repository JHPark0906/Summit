#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "Camera/CameraBackgroundBehaviour.h"
#include "Assets/AssetDatabase.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Platform/IAudioOutput.h"
#include "Platform/ITextMeasure.h"
#include "Platform/DirectoryContentSource.h"
#include "Rendering/QuadDrawGeometry.h"
#include "Rendering/RenderFrameBuilder.h"
#include "Runtime/Camera.h"
#include "Runtime/Game.h"
#include "Runtime/GameObject.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Runtime/SpriteRenderer.h"
#include "Runtime/Transform.h"
#include "SceneRendering/SceneRenderPass.h"
#include "Serialization/SceneSerializer.h"

namespace
{
void ProductionSceneProducesItsBackgroundSprite()
{
    namespace Runtime = GameEngine::Runtime;
    namespace Rendering = GameEngine::Rendering;
    namespace Math = GameEngine::Math;
    const auto require = [](const bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    const GameEngine::Platform::DirectoryContentSource content{ SUMMIT_TEST_CONTENT_DIRECTORY };
    Runtime::Game game{ nullptr, nullptr };
    require(game.GetAssetDatabase().Refresh(content), "production background assets could not be registered");
    std::vector<std::byte> bytes;
    require(content.Read("Scenes/InGame.scene", bytes), "production background scene could not be read");
    auto scene = GameEngine::Serialization::SceneSerializer::LoadFromBytes(
        bytes, "Scenes/InGame.scene", game.GetRuntimeContext());
    require(scene != nullptr, "production background scene could not be loaded");
    const auto* cameraObject = scene->FindGameObject("MainCamera");
    const auto* camera = cameraObject ? cameraObject->GetComponent<Runtime::Camera>() : nullptr;
    require(camera && camera->GetProjectionMode() == Runtime::Camera::ProjectionMode::Orthographic &&
        std::abs(camera->GetOrthographicSize() - 11.25f) < 0.001f,
        "production camera must show another 50 percent more world in each axis");
    const auto* background = scene->FindGameObject("Background");
    const auto* renderer = background ? background->GetComponent<Runtime::SpriteRenderer>() : nullptr;
    // A mathematically correct transform still produces no draw if the scene encoded the
    // asset as an unsupported JSON object instead of the engine's GUID string.
    require(renderer && renderer->GetSprite().IsValid() && renderer->GetSprite().IsGuidReference(),
        "production background lost its GUID sprite reference during scene deserialization");
    const auto texture = game.GetAssetDatabase().LoadTexture(renderer->GetSprite());
    require(texture && texture->IsValid() && texture->width == 2048 && texture->height == 1024,
        "production background GUID did not resolve to its real image");
    require(game.GetSceneManager().AddScene(std::move(scene)) != 0, "production background scene could not be adopted");
    GameEngine::SceneRendering::SceneRenderPass frontend{ nullptr };
    for (const float aspect : { 1.0f, 2.4f })
    {
        game.SetRenderSurfaceSize(800.0f * aspect, 800.0f);
        game.SetRenderAspectRatio(aspect);
        game.Update(0.0f);
        Rendering::RenderFrameBuilder builder;
        builder.SetRenderTargetSize({ static_cast<unsigned int>(800.0f * aspect), 800 });
        frontend.Collect(game, builder);
        const auto frame = std::move(builder).Build();
        const auto& packets = frame.GetDrawPackets(Rendering::RenderPass::Transparent);
        require(!packets.empty() && packets.front().instanceId == renderer->GetInstanceId() &&
                packets.front().sortingOrder == -1000,
            "production background was culled, omitted, or drawn after foreground sprites");
        const auto* draw = std::get_if<Rendering::SpriteDraw>(&packets.front().payload);
        require(draw != nullptr, "production background did not produce a sprite draw");
        const auto* material = frame.GetMaterial(draw->material);
        require(material && material->baseColorTexture && material->baseColorTexture->id == texture->id,
            "production background packet does not carry the decoded image");
        const auto quad = Rendering::TryBuildSpriteQuad(frame, *draw,
            { texture->width, texture->height }, "Summit background test");
        require(quad.has_value(), "production background sprite cannot produce a renderable quad");
        for (const Math::Vector3 corner : std::array<Math::Vector3, 4>{
            Math::Vector3{ -0.5f, -0.5f, 0 }, Math::Vector3{ -0.5f, 0.5f, 0 },
            Math::Vector3{ 0.5f, -0.5f, 0 }, Math::Vector3{ 0.5f, 0.5f, 0 } })
        {
            const auto projected = quad->worldViewProjection.TransformPoint(corner);
            require(std::abs(projected.GetX()) >= 0.999f && std::abs(projected.GetY()) >= 0.999f &&
                    projected.GetZ() > 0.0f && projected.GetZ() < 1.0f,
                "production background render quad does not cover the viewport inside the clipping planes");
        }
    }
}
}

void RunCameraBackgroundTests()
{
    ProductionSceneProducesItsBackgroundSprite();
    namespace Runtime = GameEngine::Runtime;
    namespace Math = GameEngine::Math;
    const auto require = [](const bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    const auto near = [](const float left, const float right)
    {
        return std::abs(left - right) < 0.001f;
    };

    Runtime::Game game{ nullptr, nullptr };
    Runtime::Scene scene(game.GetRuntimeContext(), "Camera background");
    auto* player = scene.CreateGameObject("Player");
    player->GetTransform().SetPosition({ 12.0f, 6.0f, 0.0f });
    auto* cameraObject = scene.CreateGameObject("MainCamera");
    require(cameraObject->GetTransform().SetParent(&player->GetTransform()),
        "camera must attach to its moving player");
    cameraObject->GetTransform().SetPosition({ 0.0f, 0.0f, -5.0f });
    auto* camera = cameraObject->AddComponent<Runtime::Camera>();
    camera->SetProjectionMode(Runtime::Camera::ProjectionMode::Orthographic);
    auto* background = scene.CreateGameObject("Background");
    require(background->GetTransform().SetParent(&cameraObject->GetTransform()),
        "background must attach directly to the camera");
    auto* cover = background->AddComponent<Summit::CameraBackgroundBehaviour>();
    cover->SetNativeSize({ 20.48f, 10.24f });

    for (const float aspect : { 1.0f, 2.4f })
    {
        camera->SetAspectRatio(aspect);
        for (const float zoom : { 5.0f, 11.25f })
        {
            camera->SetOrthographicSize(zoom);
            scene.Update(0.0f);

            const Math::Vector3 scale = background->GetTransform().GetScale();
            require(near(scale.GetX(), scale.GetY()) && scale.GetX() > 0.0f,
                "cover must retain the image aspect ratio with one positive uniform scale");
            const Math::Matrix4x4 localToClip =
                background->GetTransform().GetLocalToWorldMatrix() *
                camera->GetViewMatrix() * camera->GetProjectionMatrix();
            for (const Math::Vector3 corner : std::array<Math::Vector3, 4>{
                Math::Vector3{ -10.24f, -5.12f, 0.0f },
                Math::Vector3{ -10.24f, 5.12f, 0.0f },
                Math::Vector3{ 10.24f, -5.12f, 0.0f },
                Math::Vector3{ 10.24f, 5.12f, 0.0f } })
            {
                const Math::Vector3 projected = localToClip.TransformPoint(corner);
                const float x = std::abs(projected.GetX());
                const float y = std::abs(projected.GetY());
                require(x >= 0.999f && y >= 0.999f,
                    "all image edges must cover the viewport after resize and zoom");
                require(near(x, 1.0f) || near(y, 1.0f),
                    "cover must crop only the overflow axis without needless extra zoom");
                require(projected.GetZ() > 0.0f && projected.GetZ() < 1.0f,
                    "background must remain inside the camera clipping planes");
            }
        }
    }

    // Physics moves the player after behaviours update. No second behaviour tick may be
    // needed for the background to share the final camera position or rotation.
    player->GetTransform().SetPosition({ -40.0f, 25.0f, 0.0f });
    player->GetTransform().SetRotation({ 0.0f, 0.0f, 17.0f });
    const Math::Vector3 cameraSpaceCenter = camera->GetViewMatrix().TransformPoint(
        background->GetTransform().GetWorldPosition());
    require(near(cameraSpaceCenter.GetX(), 0.0f) && near(cameraSpaceCenter.GetY(), 0.0f),
        "background must follow post-update camera parent movement without a frame of lag");
}
