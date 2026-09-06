#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Assets/AssetDatabase.h"
#include "Gameplay/PlayerController.h"
#include "Network/MultiplayerController.h"
#include "Platform/DirectoryContentSource.h"
#include "Platform/IAudioOutput.h"
#include "Platform/ITextMeasure.h"
#include "Runtime/BoxCollider2D.h"
#include "Runtime/Game.h"
#include "Runtime/GameObject.h"
#include "Runtime/Rigidbody2D.h"
#include "Runtime/Scene.h"
#include "Runtime/SceneManager.h"
#include "Runtime/TilemapCollider2D.h"
#include "Runtime/TilemapRenderer.h"
#include "Runtime/Transform.h"
#include "Serialization/SceneSerializer.h"

namespace
{
namespace Runtime = GameEngine::Runtime;

void Require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(const float first, const float second)
{
    return std::abs(first - second) < .001f;
}
}

void RunOneWayPlatformTests()
{
    GameEngine::Platform::DirectoryContentSource content{ SUMMIT_TEST_CONTENT_DIRECTORY };
    Runtime::Game game(nullptr, nullptr);
    Require(game.GetAssetDatabase().Refresh(content), "platform test could not register production assets");
    std::vector<std::byte> bytes;
    Require(content.Read("Scenes/InGame.scene", bytes), "platform test could not read the game scene");
    auto loaded = GameEngine::Serialization::SceneSerializer::LoadFromBytes(
        bytes, "Scenes/InGame.scene", game.GetRuntimeContext());
    Require(loaded != nullptr, "platform test could not load the game scene");
    auto* player = loaded->FindGameObject("Player");
    auto* tileObject = loaded->FindGameObject("TileMap");
    Require(player && tileObject, "production scene must contain its player and tilemap");
    auto* tile = tileObject->GetComponent<Runtime::TilemapCollider2D>();
    auto* map = tileObject->GetComponent<Runtime::TilemapRenderer>();
    auto* body = player->GetComponent<Runtime::Rigidbody2D>();
    auto* collider = player->GetComponent<Runtime::BoxCollider2D>();
    Require(tile && map && body && collider && tile->IsOneWay() && !tile->IsTrigger(),
        "Summit's authored tilemap must enable one-way solid platform collision");
    // Drive the real player body through the real authored terrain, independently of input.
    player->GetComponent<Summit::PlayerController>()->SetEnabled(false);
    loaded->FindGameObject("NetworkManager")->GetComponent<Summit::MultiplayerController>()->SetEnabled(false);
    Require(game.GetSceneManager().AddScene(std::move(loaded)) != 0, "platform scene could not be adopted");
    game.Update(0);

    int topRow = -1;
    for (int row = 0; row < map->GetRows(); ++row)
        if (map->GetTile(2, row) != Runtime::TilemapRenderer::EmptyTile) topRow = row;
    Require(topRow >= 1, "the real platform fixture requires its stacked terrain column");
    const auto topPoint = tileObject->GetTransform().GetLocalToWorldMatrix().TransformPoint(
        { 2.5f * map->GetCellSize().GetX(), (topRow + 1) * map->GetCellSize().GetY(), 0 });
    const auto bounds = tile->GetWorldBounds();
    auto& transform = player->GetTransform();
    const auto place = [&](const float x, const float y, const float vx, const float vy)
    {
        Require(transform.SetWorldPosition({ x, y, 0 }), "platform fixture could not place its player");
        body->ResetMotion();
        body->SetGravityScale(0);
        body->SetVelocity({ vx, vy });
    };
    const auto step = [&] { game.Update(1.0f / 60.0f); };

    place(topPoint.GetX(), bounds.min.GetY() - 2, 0, 60);
    for (int frame = 0; frame < 7; ++frame)
    {
        step();
        Require(Near(body->GetVelocity().GetY(), 60) && !body->HasContact(Runtime::Rigidbody2DContact::Above),
            "jumping upward was blocked by the underside of the one-way map");
    }
    Require(collider->GetWorldBounds().min.GetY() > topPoint.GetY(),
        "the upward player never passed completely above the platform");
    body->SetVelocity({ 0, -60 });
    for (int frame = 0; frame < 8 && !body->IsGrounded(); ++frame) step();
    Require(body->IsGrounded() && Near(body->GetVelocity().GetY(), 0) &&
        Near(collider->GetWorldBounds().min.GetY(), topPoint.GetY()),
        "falling back from above did not land on the platform's exposed top");
    body->SetGravityScale(1);
    for (int frame = 0; frame < 30; ++frame)
    {
        step();
        Require(body->IsGrounded() && Near(collider->GetWorldBounds().min.GetY(), topPoint.GetY()),
            "resting under gravity slipped through the one-way surface");
    }

    place(bounds.min.GetX() - 2, .6f, 90, 0);
    const float horizontalTravel = bounds.max.GetX() - collider->GetWorldBounds().min.GetX();
    const int horizontalFrames = static_cast<int>(std::ceil(horizontalTravel / (90.0f / 60.0f))) + 1;
    for (int frame = 0; frame < horizontalFrames; ++frame)
    {
        step();
        Require(Near(body->GetVelocity().GetX(), 90) && body->GetContacts() == Runtime::Rigidbody2DContact::None,
            "the side of a one-way platform blocked horizontal passage");
    }
    Require(collider->GetWorldBounds().min.GetX() > bounds.max.GetX(),
        "horizontal movement did not pass completely through the map");

    place(topPoint.GetX(), 1.8f, 0, -30);
    for (int frame = 0; frame < 8; ++frame) step();
    Require(!body->IsGrounded() && Near(body->GetVelocity().GetY(), -30) &&
        transform.GetWorldPosition().GetY() < bounds.min.GetY(),
        "entering below the top caught an internal tile seam or snapped the player upward");

    tile->SetOneWay(false);
    place(bounds.min.GetX() - 2, .6f, 90, 0);
    for (int frame = 0; frame < 4; ++frame) step();
    Require(Near(body->GetVelocity().GetX(), 0) &&
        Near(collider->GetWorldBounds().max.GetX(), bounds.min.GetX()),
        "turning off one-way mode did not restore ordinary tilemap side collision");
}
