#include "Copter.h"

// Traditional helicopter variables and functions

#if FRAME_CONFIG == HELI_FRAME

#ifndef HELI_DYNAMIC_FLIGHT_SPEED_MIN
    // we are in "dynamic flight" when the speed is over 5m/s for 2 seconds
    #define HELI_DYNAMIC_FLIGHT_SPEED_MIN      500
#endif

// counter to control dynamic flight profile
static int8_t heli_dynamic_flight_counter;

// heli_init - perform any special initialisation required
void Copter::heli_init()
{
    // pre-load stab col values as mode is initialized as Stabilize, but stabilize_init() function is not run on start-up.
    input_manager.set_use_stab_col(true);
    input_manager.set_stab_col_ramp(1.0);
}

// heli_check_dynamic_flight - updates the dynamic_flight flag based on our horizontal velocity
// should be called at 50hz
void Copter::check_dynamic_flight(void)
{
    if (motors->get_spool_state() != AP_Motors::SpoolState::THROTTLE_UNLIMITED ||
        control_mode == Mode::Number::LAND || (control_mode==Mode::Number::RTL && mode_rtl.state() == RTL_Land) || (control_mode == Mode::Number::AUTO && mode_auto.mode() == Auto_Land)) {
        heli_dynamic_flight_counter = 0;
        heli_flags.dynamic_flight = false;
        return;
    }

    bool moving = false;

    // with GPS lock use inertial nav to determine if we are moving
    if (position_ok()) {
        // get horizontal speed
        const float speed = inertial_nav.get_speed_xy();
        moving = (speed >= HELI_DYNAMIC_FLIGHT_SPEED_MIN);
    }else{
        // with no GPS lock base it on throttle and forward lean angle
        //TODO replace "throttle with collective here
        moving = (motors->get_throttle() > 0.8f || ahrs.pitch_sensor < -1500);
    }

    if (!moving && rangefinder_state.enabled && rangefinder.status_orient(ROTATION_PITCH_270) == RangeFinder::RangeFinder_Good) {
        // when we are more than 2m from the ground with good
        // rangefinder lock consider it to be dynamic flight
        moving = (rangefinder.distance_cm_orient(ROTATION_PITCH_270) > 200);
    }

    if (moving) {
        // if moving for 2 seconds, set the dynamic flight flag
        if (!heli_flags.dynamic_flight) {
            heli_dynamic_flight_counter++;
            if (heli_dynamic_flight_counter >= 100) {
                heli_flags.dynamic_flight = true;
                heli_dynamic_flight_counter = 100;
            }
        }
    }else{
        // if not moving for 2 seconds, clear the dynamic flight flag
        if (heli_flags.dynamic_flight) {
            if (heli_dynamic_flight_counter > 0) {
                heli_dynamic_flight_counter--;
            }else{
                heli_flags.dynamic_flight = false;
            }
        }
    }
}

// update_heli_control_dynamics - pushes several important factors up into AP_MotorsHeli.
// should be run between the rate controller and the servo updates.
void Copter::update_heli_control_dynamics(void)
{
    // Use Leaky_I if we are not moving fast
    attitude_control->use_leaky_i(!heli_flags.dynamic_flight);

    if (ap.land_complete || (is_zero(motors->get_desired_rotor_speed()))){
        // if we are landed or there is no rotor power demanded, decrement slew scalar
        hover_roll_trim_scalar_slew--;
    } else {
        // if we are not landed and motor power is demanded, increment slew scalar
        hover_roll_trim_scalar_slew++;
    }
    hover_roll_trim_scalar_slew = constrain_int16(hover_roll_trim_scalar_slew, 0, scheduler.get_loop_rate_hz());

    // set hover roll trim scalar, will ramp from 0 to 1 over 1 second after we think helicopter has taken off
    attitude_control->set_hover_roll_trim_scalar((float) hover_roll_trim_scalar_slew/(float) scheduler.get_loop_rate_hz());
}

// heli_update_landing_swash - sets swash plate flag so higher minimum is used when landed or landing
// should be called soon after update_land_detector in main code
void Copter::heli_update_landing_swash()
{
    switch (control_mode) {
        case Mode::Number::ACRO:
        case Mode::Number::STABILIZE:
        case Mode::Number::DRIFT:
        case Mode::Number::SPORT:
            // manual modes always uses full swash range
            motors->set_collective_for_landing(false);
            break;

        case Mode::Number::LAND:
            // landing always uses limit swash range
            motors->set_collective_for_landing(true);
            break;

        case Mode::Number::RTL:
        case Mode::Number::SMART_RTL:
            if (mode_rtl.state() == RTL_Land) {
                motors->set_collective_for_landing(true);
            }else{
                motors->set_collective_for_landing(!heli_flags.dynamic_flight || ap.land_complete || !ap.auto_armed);
            }
            break;

        case Mode::Number::AUTO:
            if (mode_auto.mode() == Auto_Land) {
                motors->set_collective_for_landing(true);
            }else{
                motors->set_collective_for_landing(!heli_flags.dynamic_flight || ap.land_complete || !ap.auto_armed);
            }
            break;

        default:
            // auto and hold use limited swash when landed
            motors->set_collective_for_landing(!heli_flags.dynamic_flight || ap.land_complete || !ap.auto_armed);
            break;
    }
}

////////////////////////////   Fix This //////////////////////////////
// HeliPilot throttles are hard-coded to 7 and 8 for twin-engine heli applications
// convert motor interlock switch's position to desired rotor speed expressed as a value from 0 to 1
// returns zero if motor interlock auxiliary switch hasn't been defined
//float Copter::get_pilot_desired_rotor_speed() const
//{
//    RC_Channel *rc_ptr = rc().find_channel_for_option(RC_Channel::AUX_FUNC::MOTOR_INTERLOCK);
//    if (rc_ptr != nullptr) {
//        return (float)rc_ptr->get_control_in() * 0.001f;
//    }
//    return 0.0f;
//}

// heli_update_rotor_speed_targets - reads pilot input and passes new rotor speed targets to heli motors object
void Copter::heli_update_rotor_speed_targets()
{

    static bool rotor_runup_complete_last = false;

    // get throttle control method
    uint8_t throttle_control_mode = motors->get_throttle_mode();

    float throttle_in = ((float)RC_Channels::rc_channel(CH_8)->get_control_in()) * 0.001f;

    switch (throttle_control_mode) {
        case THROTTLE_CONTROL_SINGLE:
            if (throttle_in > 0.01f) {
                ap.motor_interlock_switch = true;
                motors->set_desired_rotor_speed(throttle_in);
                // set rpm from rotor speed sensor
                motors->set_rpm(rpm_sensor.get_rpm(0));
            } else {
                ap.motor_interlock_switch = false;
                motors->set_desired_rotor_speed(0.0f);
            }
            break;
        case THROTTLE_CONTROL_TWIN:
            float throttle2_in = ((float)RC_Channels::rc_channel(CH_7)->get_control_in()) * 0.001f;
            if ((throttle_in > 0.01f) || (throttle2_in > 0.01f)) {
                ap.motor_interlock_switch = true;
                motors->set_desired_rotor_speed(throttle_in);
                motors->set_desired_rotor_speed_2(throttle2_in);
                // set rpm from rotor speed sensor
                motors->set_rpm(rpm_sensor.get_rpm(0));
            } else {
                ap.motor_interlock_switch = false;
                motors->set_desired_rotor_speed(0.0f);
                motors->set_desired_rotor_speed_2(0.0f);
            }
            break;
    }

    // when rotor_runup_complete changes to true, log event
    if (!rotor_runup_complete_last && motors->rotor_runup_complete()){
        Log_Write_Event(DATA_ROTOR_RUNUP_COMPLETE);
    } else if (rotor_runup_complete_last && !motors->rotor_runup_complete()){
        Log_Write_Event(DATA_ROTOR_SPEED_BELOW_CRITICAL);
    }
    rotor_runup_complete_last = motors->rotor_runup_complete();
}

#endif  // FRAME_CONFIG == HELI_FRAME
