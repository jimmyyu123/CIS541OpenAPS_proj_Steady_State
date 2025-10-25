#include "OpenAPS.h"
#include <algorithm>

// Constructor
OpenAPS::OpenAPS() {}

// Add insulin treatment
void OpenAPS::addInsulinTreatment(InsulinTreatment t) {
    treatments.push_back(t);
}

// Calculate insulin activity and IOB
// (Placeholder for Milestone 1; implement in Milestone 2)
std::pair<float, float> OpenAPS::insulin_calculations(long t) {
    float total_activity = 0.0f;
    float total_IOB = 0.0f;
    
    const float peak_frac = 75.0f / 180.0f; // based on OPENAPS PPT
    const float DIA_min = DIA;
    const float EPS       = 1e-6f;            // prevent /0

    // TODO (Milestone 2): compute activity and IOB using time, dose, duration
    // Example outline:
    // for each treatment:
    //     if (t - treat.time < treat.duration)
    //         activity += f(t - treat.time)
    //         IOB += g(t - treat.time)
    for (const auto& tr : treatments) {
        long dt = t - tr.time;

        // don't calculate future dose
        if (dt < 0) continue;

        // effective window: min(duration, DIA_min)
        float eff_window = DIA_min;
        // if (tr.duration > 0) eff_window = std::min<float>(tr.duration, DIA_min);

        // over effective window
        if (dt >= static_cast<long>(eff_window)) continue;

        float t_peak = eff_window * peak_frac;
        if (t_peak < EPS) t_peak = EPS;

        // area of bi-linear = tr.dose：
        // area = 0.5 * base * height = 0.5 * eff_window * peak = dose
        // → peak = 2*dose/eff_window
        float peak = (2.0f * tr.dose) / (eff_window + EPS);

        // 3) activity（U/min）
        float dt_f = static_cast<float>(dt);
        float act_t = 0.0f;
        if (dt_f <= t_peak) {
            // from 0 linear increase to peak
            act_t = peak * (dt_f / t_peak);
        } else {
            // from peak decrease to 0
            float down_len = eff_window - t_peak;
            if (down_len < EPS) down_len = EPS;
            act_t = peak * ((eff_window - dt_f) / down_len);
        }
        if (act_t < 0) act_t = 0.0f;

        float used = 0.0f;
        if (dt_f <= t_peak) {
            // left tringale：0.5 * (peak/t_peak) * dt^2
            used = 0.5f * (peak / t_peak) * (dt_f * dt_f);
        } else {

            float area_left = 0.5f * t_peak * peak;

            float width = dt_f - t_peak;
            used = area_left + 0.5f * (peak + act_t) * width;
        }

        // 5) IOB = dose - used, [0, dose]
        float iob = tr.dose - used;
        if (iob < 0.0f)       iob = 0.0f;
        if (iob > tr.dose)    iob = tr.dose;

        total_activity += act_t; // U/min
        total_IOB      += iob;   // U
    }

    return { total_activity, total_IOB };
}

// Predict future BG using Eventual BG algorithm
// (Placeholder for Milestone 1; full logic in Milestone 2)
// TODO: USE COB FOR EXTRA CREDITS
std::pair<float, float> OpenAPS::get_BG_forecast(float current_BG,
                                                 float activity,
                                                 float IOB
                                                ) {

    // when using this algorthism from slide, whenever have a meal naive will become -1000..
    float naive_eventual_BG = current_BG - (IOB * ISF);

    float deviation = 0.0f;
    float predBGI  = - activity * ISF * 5.0f;        // predict 5 min change
    // TODO: everytime onMqttMessage deal with CGM, update this value
    if (!isnan(prev_BG)) {
        float delta    = current_BG - prev_BG;      // actual change in 5 min
        deviation      = (30.0f / 5.0f) * (delta - predBGI); // 30 min prediction
    }

    float eventual_BG = naive_eventual_BG + deviation;

    return {naive_eventual_BG, eventual_BG};
}

// Determine basal insulin rate
// (Placeholder for Milestone 1; control logic in Milestone 2)
float OpenAPS::get_basal_rate(long t, float current_BG) {

    // TODO (Milestone 2): implement control logic based on BG levels
    // Example structure:
    // if (current_BG > target_BG) → increase basal
    // else if (current_BG < threshold_BG) → decrease basal
    // add new treatment to list
    // ---- customize variables (SET BY US) ----
    const float basal_default = 0.5f;   // U/hr
    const float basal_min     = 0.0f;   // U/hr
    const float basal_max     = 2.0f;   // U/hr
    const float kWindowMin    = 5.0f;   // control window（min）

    auto clampf = [](float v, float lo, float hi){
        return v < lo ? lo : (v > hi ? hi : v);
    };

    float basal_rate = basal_default;

    // 1) get activity / IOB，and predict BG
    auto [activity, IOB] = insulin_calculations(t);
    auto [naive_eventual_BG, eventual_BG] = get_BG_forecast(current_BG, activity, IOB);

    if (current_BG < threshold_BG || eventual_BG < threshold_BG) {
        // set insulin rate to 0
        Serial.print("Case 1:");
        basal_rate = 0.0f;

    } else if (eventual_BG >= threshold_BG && eventual_BG < target_BG) {
        if (naive_eventual_BG < 40.0f) {
            Serial.print("Case 2:");
            basal_rate = 0.0f;
        } else {
            Serial.print("Case 3:");
            // multiplication by 2 to increase hypo safety (feel free to tune)
            float insulinReq = 2.0f * (eventual_BG - target_BG) / ISF;  // U 
            basal_rate = basal_default + (insulinReq / DIA);             // U/hr
            //set rate to (current basal + insulinReq / DIA) to deliver insulinReq less insulin over DIA mins
        }

    } else if (eventual_BG >= target_BG) {
        Serial.print("Case 4:");
        float insulinReq = (eventual_BG - target_BG) / ISF / 10.0f;      // U
        basal_rate = basal_default + (insulinReq / DIA);                 // U/hr

    } else {
        // maintain
        Serial.print("Case 5:");
        basal_rate = basal_default;
    }

    // limit in the boundary
    basal_rate = clampf(basal_rate, basal_min, basal_max);

    // 3) record this treatment，calculate further IOB/Activity
    float dose_this_window = basal_rate * (kWindowMin / 60.0f);
    addInsulinTreatment(InsulinTreatment{ t, dose_this_window, static_cast<int>(kWindowMin) });

    // debug print
    Serial.print("Time="); Serial.print(t);
    Serial.print(" Current BG="); Serial.print(current_BG, 1);
    Serial.print(" Naive BG="); Serial.print(naive_eventual_BG, 1);
    Serial.print(" Eventual BG="); Serial.print(eventual_BG, 1);
    Serial.print(" Basal Rate="); Serial.println(basal_rate, 3);

    return basal_rate;
}


void OpenAPS::noteNewBG(float bg, long t_min) {
    // first time
    if (last_BG_time < 0) {
        last_BG = bg;
        last_BG_time = t_min;
        return;
    }
    long dt = t_min - last_BG_time;   // minute difference
    if (dt >= 5) {
        // if over five min, update prev_bg
        prev_BG = last_BG;
        last_BG = bg;
        last_BG_time = t_min;
    } else {
        // if less the 5 min, don't update prev_BG
        last_BG = bg;
        last_BG_time = t_min;
    }
}