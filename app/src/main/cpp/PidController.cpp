#include "PidController.h"
#include <algorithm>

PidController::PidController(float kp, float ki, float kd, float target)
    : mKp(kp), mKi(ki), mKd(kd), mTarget(target),
      mIntegral(0.0f), mPrevError(0.0f) {}

float PidController::update(float currentValue, float dt) {
    float error = mTarget - currentValue;

    // Accumulate integral with anti-windup clamp
    mIntegral += error * dt;
    mIntegral = std::clamp(mIntegral, -2.0f, 2.0f);

    // Derivative (rate of error change)
    float derivative = (dt > 0.0f) ? (error - mPrevError) / dt : 0.0f;
    mPrevError = error;

    // PID output in EV stops
    float output = mKp * error + mKi * mIntegral + mKd * derivative;

    // Clamp output to reasonable EV adjustment range
    return std::clamp(output, -3.0f, 3.0f);
}

void PidController::setTarget(float target) {
    mTarget = target;
}

void PidController::reset() {
    mIntegral = 0.0f;
    mPrevError = 0.0f;
}
