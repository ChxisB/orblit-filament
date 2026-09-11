/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RenderPass.h"

#include "details/Engine.h"
#include "details/Scene.h"

#include <filament/Engine.h>

#include <private/filament/EngineEnums.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iterator>

using namespace filament;

// Automatic instancing merges runs of consecutive commands whose PrimitiveInfo compares equal.
// Custom commands are not draws: appendCustomCommand() writes only their key, so their
// PrimitiveInfo is whatever the command arena happened to hold -- in a real frame, a stale copy
// of a draw left there by an earlier pass. Two commands carrying the same stale info compare
// equal, and the second is dropped: it never runs.
//
// This reproduces that deterministically by zeroing the arena, so the two custom commands carry
// identical (zero) info. They stand for the pair the renderer itself emits in the last channel --
// fog as a post-process in COLOR and the color-grading subpass in BLENDED, which sort next to
// each other whenever no draws separate them. Losing the second leaves the tone-mapped
// attachment at its clear value and the whole frame comes out black, sky included.
TEST(RenderPassTest, AutomaticInstancingKeepsCustomCommands) {
    FEngine* engine = downcast(Engine::create(Engine::Backend::NOOP));
    engine->setAutomaticInstancingEnabled(true);
    ASSERT_TRUE(engine->isAutomaticInstancingEnabled());

    {
        // No visible renderables, so the pass holds only the custom commands. The SoA still
        // needs one element: the pass records a summed primitive count at the end of the range.
        FScene::RenderableSoa soa;
        soa.resize(1);

        alignas(64) std::array<uint8_t, 4096> memory{};
        RenderPass::Arena arena("RenderPassTest",
                { memory.data(), memory.data() + memory.size() });

        int fog = 0;
        int colorGrading = 0;
        uint8_t const lastChannel = uint8_t(CONFIG_RENDERPASS_CHANNEL_COUNT - 1);

        RenderPass pass = RenderPassBuilder(arena)
                .commandTypeFlags(RenderPass::CommandTypeFlags::COLOR)
                .geometry(soa, { 0, 0 })
                .customCommand(lastChannel, RenderPass::Pass::COLOR,
                        RenderPass::CustomCommand::EPILOGUE, 0, [&fog] { fog++; })
                .customCommand(lastChannel, RenderPass::Pass::BLENDED,
                        RenderPass::CustomCommand::EPILOGUE, 0, [&colorGrading] { colorGrading++; })
                .build(*engine, engine->getDriverApi());

        // Neither custom command was folded into the other...
        ASSERT_EQ(std::distance(pass.begin(), pass.end()), 2);

        // ...nor turned into an instanced draw. instanceify() writes info.instanceCount on the
        // command it keeps; a custom command's info is not a draw's and must be left alone. It
        // is safe to read here only because this arena is zeroed.
        EXPECT_LE(pass.begin()[0].info.instanceCount, 1);
        EXPECT_LE(pass.begin()[1].info.instanceCount, 1);

        // ...and both are executed.
        pass.finalize(*engine, engine->getDriverApi());
        pass.getExecutor().execute(*engine, engine->getDriverApi());
        EXPECT_EQ(fog, 1);
        EXPECT_EQ(colorGrading, 1);
    }

    Engine::destroy((Engine**)&engine);
}
