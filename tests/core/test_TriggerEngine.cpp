#include <gtest/gtest.h>
#include "acquisition/TriggerEngine.h"

using namespace escope;

TEST(TriggerEngine, RisingEdgeFires) {
    TriggerConfig cfg;
    cfg.condition    = TriggerCondition::RisingEdge;
    cfg.level_v      = 1.0f;
    cfg.hysteresis_v = 0.0f;
    cfg.mode         = TriggerMode::Auto;
    TriggerEngine eng(cfg);

    auto r1 = eng.evaluate_analog(0.0,   0.5f);
    EXPECT_FALSE(r1.has_value());

    auto r2 = eng.evaluate_analog(100.0, 1.5f);
    EXPECT_TRUE(r2.has_value());
    EXPECT_NEAR(r2->timestamp_ns, 100.0, 1e-6);
}

TEST(TriggerEngine, FallingEdgeFires) {
    TriggerConfig cfg;
    cfg.condition    = TriggerCondition::FallingEdge;
    cfg.level_v      = 1.0f;
    cfg.hysteresis_v = 0.0f;
    TriggerEngine eng(cfg);

    eng.evaluate_analog(0.0,   2.0f);
    auto r = eng.evaluate_analog(100.0, 0.5f);
    EXPECT_TRUE(r.has_value());
}

TEST(TriggerEngine, SingleModeDisarmsAfterFire) {
    TriggerConfig cfg;
    cfg.mode         = TriggerMode::Single;
    cfg.condition    = TriggerCondition::RisingEdge;
    cfg.level_v      = 1.0f;
    cfg.hysteresis_v = 0.0f;
    TriggerEngine eng(cfg);

    eng.evaluate_analog(0.0, 0.5f);
    eng.evaluate_analog(1.0, 1.5f);
    EXPECT_FALSE(eng.is_armed());

    eng.evaluate_analog(2.0, 0.5f);
    eng.evaluate_analog(3.0, 1.5f);
    EXPECT_FALSE(eng.is_armed());
}
