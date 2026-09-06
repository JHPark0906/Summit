#include <cstddef>
#include <memory>
#include <vector>

#include "App/GameBootstrapRegistry.h"
#include "Diagnostics/Debug.h"
#include "Platform/IContentSource.h"
#include "Platform/ApplicationContent.h"
#include "Rendering/TextRasterizationCache.h"

namespace Summit
{
namespace
{
    class SummitBootstrap final : public GameEngine::App::IGameBootstrap
    {
    public:
        [[nodiscard]] bool Initialize(
            GameEngine::Runtime::Game&, GameEngine::Platform::IWindow&) override
        {
            return mFontsReady;
        }

        void RegisterSceneFonts(GameEngine::Rendering::TextRasterizationCache& cache) override
        {
            // 패키징된 실행 파일에서도 같은 한글 글꼴을 쓰도록 배포 콘텐츠의 바이트로 등록한다.
            std::vector<std::byte> bytes;
            mFontsReady = GameEngine::Platform::GetApplicationContent().Read(
                "Fonts/NanumSquareNeoOTF-Bd.otf", bytes) &&
                cache.RegisterFont("Summit UI", bytes);
            if (!mFontsReady)
            {
                GameEngine::Diagnostics::Debug::LogError(
                    "Summit could not load Fonts/NanumSquareNeoOTF-Bd.otf.");
            }
        }

    private:
        // 엔진이 글꼴 등록을 먼저 요청한다. 실패했다면 한글 UI가 빈 채로 게임을 시작하지 않는다.
        bool mFontsReady = false;
    };

    std::unique_ptr<GameEngine::App::IGameBootstrap> CreateBootstrap()
    {
        return std::make_unique<SummitBootstrap>();
    }

    const bool gBootstrapRegistered =
        GameEngine::App::GameBootstrapRegistry::Register(&CreateBootstrap);
}
}
