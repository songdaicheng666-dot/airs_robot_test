#include "robot_adapter_interfaces/system_info.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using robot_adapter_interfaces::MotionDescriptor;
using robot_adapter_interfaces::SystemInfoBuilder;

// Runs a descriptor list through the builder and hands back the serialized
// `motions` array, i.e. exactly what a frontend would receive.
nlohmann::json BuildMotions(std::vector<MotionDescriptor> motions) {
    SystemInfoBuilder builder;
    builder.SetMotions(std::move(motions));
    return nlohmann::json::parse(builder.Build()).at("motions");
}

}  // namespace

TEST(SystemInfoMotions, DisplayNameIsEmitted) {
    const auto motions = BuildMotions({
        {"stand_up", "stand_up", "Recover to standing posture", "站立"},
    });
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("id").get<std::string>(), "stand_up");
    EXPECT_EQ(motions[0].at("service_suffix").get<std::string>(), "stand_up");
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Recover to standing posture");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "站立");
}

TEST(SystemInfoMotions, EmptyDisplayNameFallsBackToId) {
    const auto motions = BuildMotions({
        {"lights_on", "lights/on", "Turn on lights", ""},
    });
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "lights_on");
    // The fallback must not disturb the neighbouring field.
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Turn on lights");
}

TEST(SystemInfoMotions, ThreeElementInitializerKeepsDescription) {
    // display_name must stay the LAST member of MotionDescriptor. If someone
    // reorders it before `description`, this positional initializer silently
    // routes the English text into display_name — this test is what catches
    // that. -Wmissing-field-initializers is suppressed on purpose: omitting
    // the field IS the scenario under test, and CI compiles with -Werror.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
    const std::vector<MotionDescriptor> legacy = {
        {"stand_up", "stand_up", "Recover to standing posture"},
    };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    const auto motions = BuildMotions(legacy);
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Recover to standing posture");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "stand_up");
}

TEST(SystemInfoMotions, DropRulesIgnoreDisplayName) {
    const auto motions = BuildMotions({
        {"bad id", "whatever", "invalid charset in id", "非法"},
        {"no_suffix", "", "empty service_suffix", "无后缀"},
        {"dup", "dup", "first wins", "重复一"},
        {"dup", "dup2", "second dropped", "重复二"},
        {"kept", "kept", "no label supplied", ""},
    });
    // A present display_name never rescues an invalid motion; an absent one
    // never removes a valid motion. SetMotions preserves declaration order.
    ASSERT_EQ(motions.size(), 2u);
    EXPECT_EQ(motions[0].at("id").get<std::string>(), "dup");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "重复一");
    EXPECT_EQ(motions[1].at("id").get<std::string>(), "kept");
    EXPECT_EQ(motions[1].at("display_name").get<std::string>(), "kept");
}

TEST(SystemInfoMotions, MotionsKeyAbsentWhenNeverDeclared) {
    SystemInfoBuilder builder;
    builder.SetBattery(87);
    const auto parsed = nlohmann::json::parse(builder.Build());
    EXPECT_FALSE(parsed.contains("motions"));
}
