/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 The halogenOS Project
 */

package custom.refreshrate.settings;

import android.os.Bundle;
import android.os.SystemProperties;
import android.provider.Settings;

import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.widget.SettingsBasePreferenceFragment;

public class RefreshRateFragment extends SettingsBasePreferenceFragment
        implements Preference.OnPreferenceChangeListener {

    private static final String KEY_SMOOTH_DISPLAY = "smooth_display";
    private static final String KEY_LTPO_MODE = "ltpo_mode";
    private static final String PROP_SFM_MODE = "persist.sys.sfm.mode";

    private static final float PEAK_REFRESH_RATE = 120f;
    private static final float DEFAULT_REFRESH_RATE = 60f;

    private SwitchPreferenceCompat mSmoothDisplayPref;
    private ListPreference mLtpoModePref;

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        setPreferencesFromResource(R.xml.refresh_rate_settings, rootKey);

        mSmoothDisplayPref = findPreference(KEY_SMOOTH_DISPLAY);
        mLtpoModePref = findPreference(KEY_LTPO_MODE);

        String mode = SystemProperties.get(PROP_SFM_MODE, "off");
        if (!"off".equals(mode) && !"idle".equals(mode) && !"vrr".equals(mode)) {
            mode = "off";
        }
        mLtpoModePref.setValue(mode);
        updateLtpoSummary(mode);
        mLtpoModePref.setOnPreferenceChangeListener(this);

        float peakRate = Settings.System.getFloat(
                requireContext().getContentResolver(),
                Settings.System.PEAK_REFRESH_RATE, DEFAULT_REFRESH_RATE);
        boolean smoothOn = Math.round(peakRate) >= Math.round(PEAK_REFRESH_RATE)
                || Float.isInfinite(peakRate);
        mSmoothDisplayPref.setChecked(smoothOn);
        mSmoothDisplayPref.setOnPreferenceChangeListener(this);

        updateSmoothDisplayState(!"off".equals(mode));
    }

    @Override
    public boolean onPreferenceChange(Preference preference, Object newValue) {
        if (KEY_LTPO_MODE.equals(preference.getKey())) {
            String mode = (String) newValue;
            boolean active = !"off".equals(mode);

            SystemProperties.set(PROP_SFM_MODE, mode);

            if (active) {
                // SFM requires 120Hz base rate — lock min and peak to 120Hz.
                // Sub-120Hz rates are handled by skip frame mode, not mode switching.
                Settings.System.putFloat(
                        requireContext().getContentResolver(),
                        Settings.System.PEAK_REFRESH_RATE, PEAK_REFRESH_RATE);
                Settings.System.putFloat(
                        requireContext().getContentResolver(),
                        Settings.System.MIN_REFRESH_RATE, PEAK_REFRESH_RATE);
                mSmoothDisplayPref.setChecked(true);
            } else {
                Settings.System.putFloat(
                        requireContext().getContentResolver(),
                        Settings.System.MIN_REFRESH_RATE, 0f);
            }

            updateLtpoSummary(mode);
            updateSmoothDisplayState(active);

        } else if (KEY_SMOOTH_DISPLAY.equals(preference.getKey())) {
            boolean enabled = (boolean) newValue;
            Settings.System.putFloat(
                    requireContext().getContentResolver(),
                    Settings.System.PEAK_REFRESH_RATE,
                    enabled ? PEAK_REFRESH_RATE : DEFAULT_REFRESH_RATE);
        }

        return true;
    }

    private void updateLtpoSummary(String mode) {
        switch (mode) {
            case "idle":
                mLtpoModePref.setSummary(R.string.ltpo_mode_summary_idle);
                break;
            case "vrr":
                mLtpoModePref.setSummary(R.string.ltpo_mode_summary_vrr);
                break;
            default:
                mLtpoModePref.setSummary(R.string.ltpo_mode_summary_off);
                break;
        }
    }

    private void updateSmoothDisplayState(boolean ltpoActive) {
        mSmoothDisplayPref.setEnabled(!ltpoActive);
        if (ltpoActive) {
            mSmoothDisplayPref.setSummary(R.string.smooth_display_locked_by_ltpo);
        }
    }
}
