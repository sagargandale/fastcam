#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

/**
 * PID Controller for smooth auto-exposure.
 * Prevents the jarring brightness jumps ("exposure pumping") of stock auto-exposure
 * by gradually adjusting exposure values toward a target luminance.
 */
class PidController {
public:
    /**
     * @param kp Proportional gain — reacts to current error
     * @param ki Integral gain — corrects accumulated past error
     * @param kd Derivative gain — dampens rapid changes
     * @param target Target luminance value (0.47 ≈ middle gray)
     */
    PidController(float kp, float ki, float kd, float target);

    /**
     * Compute control output based on current luminance reading.
     * @param currentValue Current average frame luminance (0.0–1.0)
     * @param dt Delta time since last update (seconds)
     * @return Control output in EV stops (positive = brighten, negative = darken)
     */
    float update(float currentValue, float dt);

    void setTarget(float target);
    void reset();

private:
    float mKp;
    float mKi;
    float mKd;
    float mTarget;
    float mIntegral;
    float mPrevError;
};

#endif // PID_CONTROLLER_H
