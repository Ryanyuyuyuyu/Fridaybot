/*
 * Host-side sanity checks for Friday's frame-rate independent motion model.
 * Build example:
 *   c++ -std=c++17 -Wall -Wextra -Werror tests/face_model_test.cpp \
 *       main/apps/app_friday/face_model.cpp -o /tmp/friday_face_model_test
 */
#include "../main/apps/app_friday/face_model.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

void advance(friday::FaceModel& model, uint32_t& now, const friday::ImuSample& imu, uint32_t durationMs)
{
    const uint32_t end = now + durationMs;
    while (now < end) {
        now += 16;
        model.update(now, imu);
        const auto& pose = model.pose();
        assert(std::isfinite(pose.gazeX));
        assert(std::isfinite(pose.gazeY));
        assert(std::isfinite(pose.leftOffsetY));
        assert(std::isfinite(pose.rightOffsetY));
        assert(pose.leftWidth > 0.0f);
        assert(pose.rightWidth > 0.0f);
        assert(pose.leftHeight > 0.0f);
        assert(pose.rightHeight > 0.0f);
        assert(pose.blinkScale >= 0.0f && pose.blinkScale <= 1.0f);
    }
}

}  // namespace

int main()
{
    friday::FaceModel model;
    friday::ImuSample still;
    uint32_t now = 1000;
    model.reset(now);

    const auto initial = model.pose();
    assert(model.expression() == friday::Expression::Idle);
    assert(initial.leftRotation > initial.rightRotation);
    assert(initial.blinkScale == 1.0f);

    // The hardware keys are spatial interactions, not static expression
    // selectors. A recoils from the upper-left corner and then stares back at
    // it; B mirrors the impact before chasing the upper-right corner.
    {
        friday::FaceModel buttonModel;
        uint32_t buttonNow = 5000;
        buttonModel.reset(buttonNow);
        buttonModel.react(friday::Reaction::ButtonA, buttonNow, -0.84f, -0.78f);
        assert(buttonModel.expression() == friday::Expression::ButtonWatch);
        advance(buttonModel, buttonNow, still, 128);
        assert(buttonModel.pose().gazeX > 5.0f);
        assert(buttonModel.pose().gazeY > 3.0f);
        advance(buttonModel, buttonNow, still, 320);
        assert(buttonModel.pose().gazeX < -7.0f);
        assert(buttonModel.pose().gazeY < -4.0f);
        advance(buttonModel, buttonNow, still, 900);
        const float watchOffset = buttonModel.pose().leftOffsetY;
        advance(buttonModel, buttonNow, still, 320);
        assert(buttonModel.expression() == friday::Expression::ButtonWatch);
        assert(std::fabs(buttonModel.pose().leftOffsetY - watchOffset) > 1.0f);
        advance(buttonModel, buttonNow, still, 1200);
        assert(buttonModel.expression() == friday::Expression::ButtonWatch);

        buttonModel.reset(buttonNow);
        buttonModel.react(friday::Reaction::ButtonB, buttonNow, 0.84f, -0.78f);
        assert(buttonModel.expression() == friday::Expression::ButtonMischief);
        advance(buttonModel, buttonNow, still, 112);
        assert(buttonModel.pose().gazeX < -4.0f);
        assert(buttonModel.pose().gazeY > 2.0f);
        advance(buttonModel, buttonNow, still, 360);
        assert(buttonModel.pose().gazeX > 8.0f);
        assert(buttonModel.pose().gazeY < -5.0f);
        advance(buttonModel, buttonNow, still, 320);
        assert(std::fabs(buttonModel.pose().leftOffsetY - buttonModel.pose().rightOffsetY) > 4.0f);
        advance(buttonModel, buttonNow, still, 2500);
        assert(buttonModel.expression() == friday::Expression::ButtonMischief);

        // A fresh input replaces a long performance immediately instead of
        // making Friday feel unresponsive until the choreography completes.
        buttonModel.react(friday::Reaction::ButtonA, buttonNow, -0.84f, -0.78f);
        assert(buttonModel.expression() == friday::Expression::ButtonWatch);
    }

    // Long performances leave an emotional afterglow instead of snapping
    // directly to idle: A remains suspicious and B celebrates the game.
    {
        friday::FaceModel afterglow;
        uint32_t afterglowNow = 1000;
        afterglow.reset(afterglowNow);
        afterglow.react(friday::Reaction::ButtonA, afterglowNow, -0.84f, -0.78f);
        advance(afterglow, afterglowNow, still, 4400);
        assert(afterglow.expression() == friday::Expression::Suspicious);
        assert(afterglow.pose().leftHeight > afterglow.pose().leftWidth);
        assert(afterglow.pose().rightHeight > afterglow.pose().rightWidth);

        afterglow.react(friday::Reaction::ButtonB, afterglowNow, 0.84f, -0.78f);
        advance(afterglow, afterglowNow, still, 4900);
        assert(afterglow.expression() == friday::Expression::Delighted);
        assert(afterglow.pose().leftHeight > afterglow.pose().leftWidth);
        assert(afterglow.pose().rightHeight > afterglow.pose().rightWidth);
    }

    // Cross-device travel has a full exit and entry performance. The eyes
    // leave through the selected bezel edge, then return from that same side
    // while preserving the vertical capsule silhouette.
    {
        friday::FaceModel portal;
        uint32_t portalNow = 1000;
        portal.reset(portalNow);
        portal.react(friday::Reaction::PortalExit, portalNow, 1.0f, 0.0f);
        advance(portal, portalNow, still, 720);
        assert(portal.expression() == friday::Expression::PortalExit);
        assert(portal.pose().gazeX > 100.0f);
        assert(portal.pose().leftHeight > portal.pose().leftWidth);
        assert(portal.pose().rightHeight > portal.pose().rightWidth);

        portal.reset(portalNow);
        portal.react(friday::Reaction::PortalReturn, portalNow, 1.0f, 0.0f);
        advance(portal, portalNow, still, 180);
        assert(portal.expression() == friday::Expression::PortalReturn);
        assert(portal.pose().gazeX > 30.0f);
        advance(portal, portalNow, still, 1050);
        assert(std::fabs(portal.pose().gazeX) < 30.0f);
        assert(portal.pose().leftHeight > portal.pose().leftWidth);
        assert(portal.pose().rightHeight > portal.pose().rightWidth);
    }

    friday::ImuSample tilted = still;
    tilted.accelX            = 0.65f;
    advance(model, now, tilted, 500);
    assert(model.pose().gazeX > 3.0f);
    assert(model.pose().gazeX < 10.0f);
    assert(model.pose().leftHeight > model.pose().rightHeight);

    model.react(friday::Reaction::Tap, now, 0.7f, 0.0f);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Surprised);
    assert(model.pose().leftHeight > initial.leftHeight);

    advance(model, now, still, 700);
    model.setTouchContact(true, 0.65f, 0.1f, now);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Curious);
    assert(model.pose().gazeX > 4.0f);
    model.setTouchContact(true, -0.65f, 0.1f, now);
    advance(model, now, still, 300);
    assert(model.pose().gazeX < 0.0f);
    model.setTouchContact(false, -0.65f, 0.1f, now);
    model.react(friday::Reaction::DragRelease, now, -0.65f, 0.1f);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Playful);

    advance(model, now, still, 1500);
    model.setTouchContact(true, 0.0f, 0.0f, now);
    advance(model, now, still, 80);
    model.setTouchContact(true, 1.0f, 0.0f, now);
    advance(model, now, still, 320);
    assert(model.expression() == friday::Expression::EdgeSquished);
    assert(model.pose().gazeX > 80.0f);
    assert(model.pose().eyeSpacing < 110.0f);
    const float edgeGaze = model.pose().gazeX;
    model.setTouchContact(false, 1.0f, 0.0f, now);
    model.react(friday::Reaction::EdgeBounce, now, 1.0f, 0.0f);
    advance(model, now, still, 260);
    assert(model.expression() == friday::Expression::Playful);
    assert(model.pose().gazeX < edgeGaze);

    advance(model, now, still, 1600);
    model.react(friday::Reaction::Swipe, now, 1.0f, 0.0f);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Excited);
    assert(model.pose().gazeX > 5.0f);

    advance(model, now, still, 1100);
    assert(model.expression() == friday::Expression::Proud);
    model.react(friday::Reaction::Pet, now);
    advance(model, now, still, 400);
    assert(model.expression() == friday::Expression::Shy);
    assert(model.pose().eyeSpacing < initial.eyeSpacing);
    assert(model.pose().leftHeight < initial.leftHeight);
    assert(model.pose().leftHeight > model.pose().leftWidth);
    assert(model.pose().rightHeight > model.pose().rightWidth);

    advance(model, now, still, 3100);
    friday::ImuSample spinning = still;
    spinning.gyroZ             = 420.0f;
    advance(model, now, spinning, 200);
    assert(model.expression() == friday::Expression::Dizzy);

    // A long delayed frame must not destabilize the spring integrator.
    now += 2000;
    model.update(now, still);
    assert(std::isfinite(model.pose().leftHeight));
    assert(model.pose().leftHeight > 0.0f);

    // With no interaction Friday should eventually sleep, then wake excited
    // when the user returns and pets the dial.
    advance(model, now, still, 50000);
    assert(model.expression() == friday::Expression::Sleepy);
    assert(model.pose().leftHeight > model.pose().leftWidth);
    assert(model.pose().rightHeight > model.pose().rightWidth);
    model.react(friday::Reaction::Pet, now);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Welcome);

    // Host context enhances rather than replaces the local personality.
    model.setCompanionContext({friday::CompanionState::Working, -1}, now);
    advance(model, now, still, 1900);
    assert(model.expression() != friday::Expression::Sleepy);

    model.setCompanionContext({friday::CompanionState::Away, -1}, now);
    advance(model, now, still, 7000);
    assert(model.expression() == friday::Expression::Sleepy);

    model.setCompanionContext({friday::CompanionState::Returned, -1}, now);
    advance(model, now, still, 240);
    assert(model.expression() == friday::Expression::Welcome);

    model.setCompanionContext({friday::CompanionState::Offline, 0}, now);
    advance(model, now, still, 47000);
    assert(model.expression() == friday::Expression::Sleepy);

    // Coarse desktop context selects a mood, while Friday owns the detailed
    // performance. Looking right creates a curved-dial turn: the near eye is
    // larger and the far eye is compressed. The same Listening mood later
    // advances to another capsule pose instead of remaining a static icon.
    friday::FaceModel contextual;
    uint32_t contextualNow = 1000;
    contextual.reset(contextualNow);
    contextual.setCompanionContext({friday::CompanionState::Meeting, 1}, contextualNow);
    advance(contextual, contextualNow, still, 600);
    assert(contextual.expression() == friday::Expression::Listening);
    const auto listeningPose = contextual.pose();
    assert(listeningPose.rightWidth > listeningPose.leftWidth);
    assert(listeningPose.leftHeight > listeningPose.leftWidth);
    assert(listeningPose.rightHeight > listeningPose.rightWidth);
    advance(contextual, contextualNow, still, 5200);
    assert(contextual.expression() == friday::Expression::Listening);
    assert(std::fabs(contextual.pose().leftHeight - listeningPose.leftHeight) > 3.0f ||
           std::fabs(contextual.pose().rightHeight - listeningPose.rightHeight) > 3.0f);

    friday::FaceModel uncertain;
    uint32_t uncertainNow = 1000;
    uncertain.reset(uncertainNow);
    uncertain.setCompanionContext({friday::CompanionState::Uncertain, -1}, uncertainNow);
    advance(uncertain, uncertainNow, still, 500);
    assert(uncertain.expression() == friday::Expression::Thinking);
    assert(uncertain.pose().leftHeight > uncertain.pose().leftWidth);
    assert(uncertain.pose().rightHeight > uncertain.pose().rightWidth);

    // The offline personality should expose more than idle and sleep without
    // requiring a touch or desktop companion packet.
    friday::FaceModel ambient;
    uint32_t ambientNow = 1000;
    ambient.reset(ambientNow);
    bool sawAmbientExpression = false;
    for (int frame = 0; frame < 1250; ++frame) {
        ambientNow += 16;
        ambient.update(ambientNow, still);
        const auto expression = ambient.expression();
        if (expression == friday::Expression::Happy || expression == friday::Expression::Curious ||
            expression == friday::Expression::Sad || expression == friday::Expression::Playful) {
            sawAmbientExpression = true;
        }
    }
    assert(sawAmbientExpression);

    // Three quick pokes produce a sustained annoyed response instead of
    // replaying the same surprised boop forever.
    friday::FaceModel annoyed;
    uint32_t annoyedNow = 1000;
    annoyed.reset(annoyedNow);
    annoyed.react(friday::Reaction::Tap, annoyedNow, 0.7f, 0.0f);
    advance(annoyed, annoyedNow, still, 700);
    annoyed.react(friday::Reaction::Tap, annoyedNow, 0.7f, 0.0f);
    advance(annoyed, annoyedNow, still, 700);
    annoyed.react(friday::Reaction::Tap, annoyedNow, 0.7f, 0.0f);
    advance(annoyed, annoyedNow, still, 240);
    assert(annoyed.expression() == friday::Expression::Annoyed);
    assert(annoyed.pose().leftHeight > annoyed.pose().rightHeight);
    assert(annoyed.pose().gazeX < 0.0f);
    advance(annoyed, annoyedNow, still, 3400);
    assert(annoyed.expression() == friday::Expression::Sulky);
    assert(annoyed.pose().leftHeight > annoyed.pose().leftWidth);
    assert(annoyed.pose().rightHeight > annoyed.pose().rightWidth);

    // Gravity postures are held long enough to reject sensor noise, but react
    // to normal wrist motion rather than only extreme spins.
    friday::FaceModel posture;
    uint32_t postureNow = 1000;
    posture.reset(postureNow);
    friday::ImuSample gentleTilt = still;
    gentleTilt.accelX = 0.34f;
    gentleTilt.accelZ = 0.94f;
    advance(posture, postureNow, gentleTilt, 520);
    assert(posture.expression() == friday::Expression::Peeking);

    friday::ImuSample steepTilt = still;
    steepTilt.accelX = 0.82f;
    steepTilt.accelZ = 0.57f;
    advance(posture, postureNow, steepTilt, 520);
    assert(posture.expression() == friday::Expression::Braced);

    friday::ImuSample upsideDown = still;
    upsideDown.accelZ = -1.0f;
    advance(posture, postureNow, upsideDown, 900);
    assert(posture.expression() == friday::Expression::UpsideDown);

    friday::FaceModel floating;
    uint32_t floatingNow = 1000;
    floating.reset(floatingNow);
    friday::ImuSample lowG = still;
    lowG.accelZ = 0.35f;
    advance(floating, floatingNow, lowG, 220);
    assert(floating.expression() == friday::Expression::Floating);

    friday::FaceModel impact;
    uint32_t impactNow = 1000;
    impact.reset(impactNow);
    friday::ImuSample setDown = still;
    setDown.accelZ = 1.55f;
    advance(impact, impactNow, setDown, 180);
    assert(impact.expression() == friday::Expression::Startled);

    friday::FaceModel shaken;
    uint32_t shakenNow = 1000;
    shaken.reset(shakenNow);
    bool sawShakeAnnoyance = false;
    for (int direction = 0; direction < 7; ++direction) {
        friday::ImuSample shake = still;
        shake.gyroX = direction % 2 == 0 ? 92.0f : -92.0f;
        advance(shaken, shakenNow, shake, 160);
        sawShakeAnnoyance |= shaken.expression() == friday::Expression::Annoyed;
    }
    assert(sawShakeAnnoyance);

    friday::FaceModel rocked;
    uint32_t rockedNow = 1000;
    rocked.reset(rockedNow);
    bool sawRocking = false;
    for (int direction = 0; direction < 8; ++direction) {
        friday::ImuSample rock = still;
        rock.accelX = direction % 2 == 0 ? 0.38f : -0.38f;
        rock.accelZ = 0.925f;
        rock.gyroY  = direction % 2 == 0 ? 28.0f : -28.0f;
        advance(rocked, rockedNow, rock, 340);
        sawRocking |= rocked.expression() == friday::Expression::Rocking;
    }
    assert(sawRocking);

    return 0;
}
