// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceGroup;
import androidx.preference.PreferenceScreen;
import dev.oneuiproject.oneui.layout.ToolbarLayout;
import dev.oneuiproject.oneui.preference.TipsCardPreference;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** A native OneUIProject settings surface over the module's shared APIs. */
public final class MainActivity extends AppCompatActivity {
    private final RootApi api = new RootApi();
    private final ExecutorService workers = Executors.newFixedThreadPool(2);
    private final Handler ui = new Handler(Looper.getMainLooper());
    private JSONObject snapshot = new JSONObject(), controls = new JSONObject();
    private JSONObject playback = new JSONObject(), availability = new JSONObject();
    private ToolbarLayout toolbar;
    private SettingsFragment fragment;
    private String page = "home", error, errorKind, notice, archiveToSave;
    private String selectedFamily = "adaptive", selectedMode = "hq";
    private int selectedRate = 48000, selectedBits = 24;
    private boolean refreshing, working, capturing, resumed;
    private long updatedAt;
    private interface Work { String run() throws Exception; }
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (!resumed) return;
            refresh(false);
            ui.postDelayed(this, 4000);
        }
    };

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        api.bind(getIntent().getStringExtra("shizuka_socket"));
        if (saved != null) page = saved.getString("page", "home");
        setContentView(R.layout.activity_main);
        toolbar = findViewById(R.id.toolbar_layout);
        ViewCompat.setOnApplyWindowInsetsListener(toolbar, (view, insets) -> {
            androidx.core.graphics.Insets bars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return WindowInsetsCompat.CONSUMED;
        });
        ViewCompat.requestApplyInsets(toolbar);
        getSupportFragmentManager().beginTransaction().replace(R.id.settings_content, new SettingsFragment()).commitNow();
        toolbar.setNavigationButtonAsBack();
        toolbar.setNavigationButtonOnClickListener(view -> show("home"));
        render();
        refresh(true);
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent); setIntent(intent);
        api.bind(intent.getStringExtra("shizuka_socket"));
        error = null; errorKind = null; notice = null; refreshing = false;
        snapshot = new JSONObject(); controls = new JSONObject(); playback = new JSONObject();
        availability = new JSONObject(); updatedAt = 0; render(); refresh(true);
    }
    @Override protected void onResume() { super.onResume(); resumed = true; ui.postDelayed(poll, 4000); }
    @Override protected void onPause() { resumed = false; ui.removeCallbacks(poll); super.onPause(); }
    @Override protected void onDestroy() { ui.removeCallbacks(poll); workers.shutdown(); super.onDestroy(); }
    @Override protected void onSaveInstanceState(Bundle out) { out.putString("page", page); super.onSaveInstanceState(out); }
    @Override public void onBackPressed() { if (!page.equals("home")) show("home"); else super.onBackPressed(); }
    @Override public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, 1, 0, "Refresh").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        menu.add(0, 2, 1, "Copy status").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        return true;
    }
    @Override public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == 1) { refresh(true); return true; }
        if (item.getItemId() == 2) { copy("Shizuka status", pretty(snapshot)); return true; }
        return super.onOptionsItemSelected(item);
    }
    private void show(String next) { page = next; notice = null; render(); if (next.equals("home")) toolbar.setExpanded(true, false); refresh(next.equals("playback") || next.equals("tuning")); }
    private void render() {
        if (isFinishing() || isDestroyed() || toolbar == null) return;
        String title = page.equals("home") ? "Shizuka" : page.equals("playback") ? "Playback"
            : page.equals("tuning") ? "Additional delay" : page.equals("dsp") ? "DSP status"
            : page.equals("diagnostics") ? "Diagnostics" : page.equals("module") ? "Module" : "About Shizuka";
        toolbar.setTitle(title);
        toolbar.setExpandedSubtitle(page.equals("home") ? "Your Bluetooth workspace" : null);
        toolbar.setNavigationButtonVisible(!page.equals("home"));
        toolbar.setExpandable(page.equals("home"));
        if (fragment != null && fragment.isAdded()) fragment.rebuild();
    }
    private void refresh(boolean full) {
        if (refreshing || isFinishing() || isDestroyed()) return;
        refreshing = true;
        final long requestGeneration = api.generation();
        workers.execute(() -> {
            JSONObject fresh = null, caps = null, playing = null, possible = null;
            String failed = null, failedKind = null;
            try {
                fresh = api.json("status", 20);
                if (full || controls.length() == 0) {
                    caps = api.json("config", 20, "tuning", "capabilities");
                    try { possible = api.json("config", 20, "tuning", "availability"); } catch (Exception ignored) { }
                }
                if (page.equals("playback") && full) {
                    try { playing = api.json("playback", 20, "get", "active"); }
                    catch (Exception e) { playing = object("available", false, "error", clean(e)); }
                }
            } catch (Exception e) { failed = clean(e); failedKind = e instanceof RootApi.Unavailable ? ((RootApi.Unavailable)e).kind : "connection"; }
            final JSONObject a = fresh, b = caps, c = playing, d = possible;
            final String message = failed, kind = failedKind;
            ui.post(() -> {
                if (isDestroyed() || requestGeneration != api.generation()) return;
                refreshing = false; error = message; errorKind = kind;
                if (message != null) {
                    snapshot = new JSONObject(); controls = new JSONObject(); playback = new JSONObject();
                    availability = new JSONObject(); updatedAt = 0; page = "home";
                    render(); return;
                }
                if (a != null) { snapshot = a; updatedAt = System.currentTimeMillis(); }
                if (b != null) controls = b;
                if (d != null) availability = d;
                if (c != null) { playback = c; readSelectedPlayback(); }
                render();
            });
        });
    }
    private void readSelectedPlayback() {
        JSONObject observed = playback.optJSONObject("observed");
        if (observed == null) return;
        String family = observed.optString("family", "other");
        if (family.equals("adaptive") || family.equals("lossless")) selectedFamily = family;
        selectedMode = observed.optString("mode", "hq").toLowerCase(Locale.ROOT);
        selectedRate = first(observed.optJSONArray("sample_rates_hz"), selectedRate);
        selectedBits = first(observed.optJSONArray("bit_depths"), selectedBits);
    }
    private void perform(String title, Work work) {
        if (working) { toast("Wait for the current action to finish."); return; }
        working = true; notice = title + "…"; render();
        workers.execute(() -> {
            String message;
            try { message = work.run(); }
            catch (Exception e) { message = clean(e); }
            final String result = message;
            ui.post(() -> { if (isDestroyed()) return; working = false; notice = result; render(); refresh(true); });
        });
    }
    private static String clean(Exception e) {
        String message = e.getMessage();
        if (message == null || message.isEmpty()) message = e.getClass().getSimpleName();
        return message.length() > 1000 ? message.substring(0, 1000) : message;
    }
    private static JSONObject object(Object... pairs) {
        JSONObject out = new JSONObject();
        try { for (int i = 0; i < pairs.length; i += 2) out.put((String)pairs[i], pairs[i + 1]); } catch (Exception ignored) { }
        return out;
    }
    private static int first(JSONArray values, int fallback) { return values == null || values.length() == 0 ? fallback : values.optInt(0, fallback); }
    private static String pretty(JSONObject json) { try { return json.toString(2); } catch (Exception e) { return json.toString(); } }
    private void copy(String label, String text) {
        ((ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText(label, text));
        toast("Copied to clipboard");
    }
    private void toast(String text) { Toast.makeText(this, text, Toast.LENGTH_SHORT).show(); }
    private JSONObject device() { JSONArray list = snapshot.optJSONArray("devices"); return list == null || list.length() == 0 ? new JSONObject() : list.optJSONObject(0); }
    private JSONObject codec() { JSONObject d = device(); JSONObject value = d == null ? null : d.optJSONObject("codec"); return value == null ? new JSONObject() : value; }
    private String connectionName() { JSONObject d = device(); return d == null ? "No active device" : d.optString("device", "No active device"); }
    private String codecSummary() {
        JSONObject c = codec();
        if (!c.optBoolean("available")) return "Connect a Bluetooth audio device";
        String name = c.optString("name", "Unknown codec"), mode = c.optString("mode", "");
        int rate = first(c.optJSONArray("sample_rates_hz"), 0), bits = first(c.optJSONArray("bit_depths"), 0);
        return name + (mode.isEmpty() ? "" : " · " + mode) + (rate == 0 ? "" : " · " + rateLabel(rate)) + (bits == 0 ? "" : " · " + bits + " bit");
    }
    private static String rateLabel(int rate) { return rate == 44100 ? "44.1 kHz" : (rate / 1000) + " kHz"; }
    private void androidSettings() { startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)); }
    private void dialog(String title, String text) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(text)
            .setPositiveButton("Done", null).setNeutralButton("Copy", (d, w) -> copy(title, text)).show();
    }
    private void choose(String title, List<String> labels, java.util.function.IntConsumer picked) {
        if (labels.isEmpty()) { toast("This device does not report supported choices."); return; }
        new AlertDialog.Builder(this).setTitle(title).setItems(labels.toArray(new String[0]), (dialog, which) -> picked.accept(which))
            .setNegativeButton("Cancel", null).show();
    }
    private JSONObject capability(String key) {
        JSONArray all = controls.optJSONArray("controls");
        if (all != null) for (int index = 0; index < all.length(); index++) {
            JSONObject value = all.optJSONObject(index);
            if (value != null && key.equals(value.optString("key"))) return value;
        }
        return null;
    }
    private int requested(String key) {
        JSONObject tuning = snapshot.optJSONObject("tuning"), values = tuning == null ? null : tuning.optJSONObject("requested");
        return values == null ? 0 : values.optInt(key, 0);
    }
    private void editDelay(String key, String label) {
        JSONObject domain = capability(key);
        if (domain == null) { toast("The module has not reported this control."); return; }
        int min = domain.optInt("min"), max = domain.optInt("max"), step = domain.optInt("step", 1);
        LinearLayout panel = new LinearLayout(this); panel.setOrientation(LinearLayout.VERTICAL); panel.setPadding(48, 12, 48, 0);
        TextView help = new TextView(this); help.setText("Additional delay, " + min + "–" + max + " ms. Saving keeps your requested value; it does not change DSP firmware.");
        panel.addView(help);
        EditText input = new EditText(this); input.setInputType(InputType.TYPE_CLASS_NUMBER); input.setSingleLine(true);
        input.setText(Integer.toString(requested(key))); input.setSelectAllOnFocus(true); input.setContentDescription(label + " in milliseconds"); panel.addView(input);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(label).setView(panel).setNegativeButton("Cancel", null).setPositiveButton("Save", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
            try {
                String text = input.getText().toString().trim();
                if (!text.matches("0|[1-9][0-9]*")) throw new NumberFormatException();
                int value = Integer.parseInt(text);
                if (value < min || value > max || (value - min) % step != 0) throw new NumberFormatException();
                dialog.dismiss(); perform("Saving requested delay", () -> {
                    api.json("config", 20, "tuning", "set", key, text);
                    return "Saved +" + value + " ms. DSP application remains separate.";
                });
            } catch (Exception invalid) { input.setError("Enter " + min + "–" + max + " in steps of " + step + " ms"); }
        }));
        dialog.show();
    }
    private JSONObject selectedCapability() {
        JSONArray list = playback.optJSONArray("selectable");
        if (list != null) for (int i = 0; i < list.length(); i++) {
            JSONObject item = list.optJSONObject(i);
            if (item != null && selectedFamily.equals(item.optString("family"))) return item;
        }
        return new JSONObject();
    }
    private void chooseMode() {
        JSONArray all = playback.optJSONArray("selectable");
        List<String> labels = new ArrayList<>(), families = new ArrayList<>(), modes = new ArrayList<>();
        if (all != null) for (int i = 0; i < all.length(); i++) {
            JSONObject item = all.optJSONObject(i); if (item == null) continue;
            String family = item.optString("family"); JSONArray supported = item.optJSONArray("modes");
            if (family.equals("adaptive") && supported != null) for (int j = 0; j < supported.length(); j++) {
                String mode = supported.optString(j);
                labels.add(mode.equals("ll") ? "Adaptive LL · lower latency" : "Adaptive HQ · higher quality"); families.add(family); modes.add(mode);
            }
            if (family.equals("lossless")) { labels.add("Lossless · 44.1 kHz / 16 bit"); families.add(family); modes.add("hq"); }
        }
        choose("Playback mode", labels, index -> {
            selectedFamily = families.get(index); selectedMode = modes.get(index);
            JSONObject cap = selectedCapability();
            selectedRate = first(cap.optJSONArray("sample_rates_hz"), selectedFamily.equals("lossless") ? 44100 : 48000);
            selectedBits = first(cap.optJSONArray("bit_depths"), selectedFamily.equals("lossless") ? 16 : 24); render();
        });
    }
    private void chooseNumbers(boolean rates) {
        JSONArray values = selectedCapability().optJSONArray(rates ? "sample_rates_hz" : "bit_depths");
        List<String> labels = new ArrayList<>(); List<Integer> numbers = new ArrayList<>();
        if (values != null) for (int i = 0; i < values.length(); i++) { int value = values.optInt(i); numbers.add(value); labels.add(rates ? rateLabel(value) : value + " bit"); }
        choose(rates ? "Sample rate" : "Bit depth", labels, index -> { if (rates) selectedRate = numbers.get(index); else selectedBits = numbers.get(index); render(); });
    }
    private void applyPlayback() {
        if (selectedCapability().length() == 0) { toast("Connect an A2DP device and refresh its capabilities first."); return; }
        final String family = selectedFamily, mode = selectedMode, rate = Integer.toString(selectedRate), bits = Integer.toString(selectedBits);
        perform("Requesting playback preference", () -> {
            JSONObject result = api.json("playback", 25, "select", "active", family, mode, rate, bits);
            return result.optBoolean("observation_matches_request")
                ? "Android reports the requested playback preference. DSP state is shown separately."
                : "Request sent. Android has not yet confirmed the selected playback preference.";
        });
    }
    private void capture(int duration) {
        if (capturing) { toast("A diagnostic capture is already running."); return; }
        capturing = true; notice = "Capturing DSP logs for " + duration + " seconds…"; render();
        workers.execute(() -> {
            String message;
            try {
                JSONObject result = api.json("diagnostics", duration + 35, "capture", Integer.toString(duration));
                message = "Capture completed · " + result.optLong("recognized_records") + " encoder records. Open saved captures to export.";
            } catch (Exception e) { message = clean(e); }
            final String done = message;
            ui.post(() -> { if (isDestroyed()) return; capturing = false; notice = done; render(); refresh(false); });
        });
    }
    private void sessions() {
        perform("Loading saved captures", () -> {
            JSONArray list = api.json("diagnostics", 20, "list").optJSONArray("sessions");
            List<String> ids = new ArrayList<>();
            if (list != null) for (int i = list.length() - 1; i >= 0; i--) ids.add(list.optString(i));
            ui.post(() -> choose("Saved captures", ids, index -> export(ids.get(index))));
            return ids.isEmpty() ? "No saved captures yet." : "Choose a completed capture to save a ZIP.";
        });
    }
    private void export(String id) {
        if (!id.matches("session-[a-f0-9-]{36}")) return;
        perform("Preparing diagnostic ZIP", () -> {
            String archive = api.json("diagnostics", 35, "export", id).getString("archive");
            ui.post(() -> {
                archiveToSave = archive;
                Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                    .putExtra(Intent.EXTRA_TITLE, "Shizuka-" + id + ".zip");
                startActivityForResult(save, 8);
            });
            return "Choose where to save the diagnostic ZIP.";
        });
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 8 && result == RESULT_OK && data != null && data.getData() != null && archiveToSave != null) {
            final Uri target = data.getData(); final String archive = archiveToSave; archiveToSave = null;
            perform("Saving diagnostic ZIP", () -> {
                try (OutputStream output = getContentResolver().openOutputStream(target, "w")) {
                    if (output == null) throw new IllegalStateException("The selected document could not be opened");
                    api.copyArchive(archive, output);
                }
                return "Diagnostic ZIP saved.";
            });
        }
    }

    public static final class SettingsFragment extends PreferenceFragmentCompat {
        private MainActivity host;
        private String renderedPage;
        @Override public void onCreatePreferences(Bundle saved, String key) {
            host = (MainActivity)requireActivity(); host.fragment = this; rebuild();
        }
        @Override public void onViewCreated(View view, Bundle saved) {
            super.onViewCreated(view, saved);
            view.setBackgroundColor(requireContext().getColor(dev.oneuiproject.oneui.design.R.color.oui_background_color));
            getListView().seslSetLastRoundedCorner(false);
        }
        private PreferenceCategory category(PreferenceScreen screen, String title) {
            PreferenceCategory category = new PreferenceCategory(requireContext()); category.setTitle(title); screen.addPreference(category); return category;
        }
        private Preference row(PreferenceGroup group, String title, String summary, Runnable action) {
            Preference pref = new Preference(requireContext()); pref.setTitle(title); pref.setSummary(summary); pref.setPersistent(false); pref.setIconSpaceReserved(false);
            if (action != null) pref.setOnPreferenceClickListener(p -> { action.run(); return true; }); else pref.setSelectable(false);
            group.addPreference(pref); return pref;
        }
        private void tips(PreferenceScreen screen, String title, String text) {
            TipsCardPreference tip = new TipsCardPreference(requireContext()); tip.setTitle(title); tip.setSummary(text); screen.addPreference(tip);
        }
        void rebuild() {
            if (host == null) return;
            final android.os.Parcelable position = host.page.equals(renderedPage) && getView() != null
                && getListView().getLayoutManager() != null
                ? getListView().getLayoutManager().onSaveInstanceState() : null;
            renderedPage = host.page;
            PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(requireContext());
            setPreferenceScreen(screen);
            if (host.error != null) {
                tips(screen, "session".equals(host.errorKind) ? "Module session unavailable" : "Shizuka module unavailable", host.error);
                row(screen, "Retry module connection", "Retry the current module session", () -> host.refresh(true));
                row(screen, "Close", "Return to KernelSU", host::finish);
                return;
            }
            if (host.updatedAt == 0) {
                tips(screen, "Opening module session", "Connecting to the temporary root session opened by KernelSU Action.");
                row(screen, "Close", "Return to KernelSU", host::finish);
                return;
            }
            if (host.notice != null) tips(screen, host.working || host.capturing ? "In progress" : "Latest action", host.notice);
            switch (host.page) {
            case "home": home(screen); break;
            case "playback": playback(screen); break;
            case "tuning": tuning(screen); break;
            case "dsp": dsp(screen); break;
            case "diagnostics": diagnostics(screen); break;
            case "module": module(screen); break;
            default: about(screen);
            }
            if (position != null) getListView().post(() -> {
                if (isAdded() && getView() != null && getListView().getLayoutManager() != null)
                    getListView().getLayoutManager().onRestoreInstanceState(position);
            });
        }
        private void home(PreferenceScreen screen) {
            String route = host.snapshot.optJSONObject("route") == null ? "Checking connection" : host.snapshot.optJSONObject("route").optString("transport", "Unknown route");
            tips(screen, host.connectionName(), host.codecSummary() + "\n" + route + (host.device().optBoolean("stream_started") ? " · Playing" : " · Idle"));
            PreferenceCategory listening = category(screen, "Listening");
            row(listening, "Playback", host.codecSummary(), () -> host.show("playback"));
            row(listening, "Additional delay", "Requested: HQ / Lossless +" + host.requested("adaptive_hq_ttp_delta_ms") + " ms · Adaptive LL +" + host.requested("adaptive_ll_ttp_delta_ms") + " ms", () -> host.show("tuning"));
            PreferenceCategory inspect = category(screen, "Inspect and debug");
            JSONObject dsp = host.snapshot.optJSONObject("dsp");
            row(inspect, "DSP status", dsp != null && dsp.optBoolean("available") ? "Current encoder observations available" : dsp == null ? "Checking DSP observations" : dsp.optString("reason", "Not collecting"), () -> host.show("dsp"));
            row(inspect, "Diagnostics", "Capture, inspect and export DSP logs", () -> host.show("diagnostics"));
            PreferenceCategory app = category(screen, "Shizuka");
            row(app, "Module", "Connection, recovery and native backend", () -> host.show("module"));
            row(app, "About", "OneUIProject · Open source", () -> host.show("about"));
        }
        private void playback(PreferenceScreen screen) {
            tips(screen, "Current playback", host.connectionName() + "\n" + host.codecSummary());
            JSONObject connected = host.playback.optJSONObject("device");
            boolean ready = connected != null && connected.optBoolean("connected");
            if (!ready) {
                row(screen, "Connect a Bluetooth device", host.playback.optString("error", "An active A2DP connection is required to select a codec."), host::androidSettings);
                row(screen, "Refresh capabilities", "Read supported choices from the connected device", () -> host.refresh(true));
                return;
            }
            PreferenceCategory request = category(screen, "Playback preference");
            row(request, "Mode", host.selectedFamily.equals("lossless") ? "Lossless" : "Adaptive " + host.selectedMode.toUpperCase(Locale.ROOT), host::chooseMode);
            row(request, "Sample rate", rateLabel(host.selectedRate), () -> host.chooseNumbers(true));
            row(request, "Bit depth", host.selectedBits + " bit", () -> host.chooseNumbers(false));
            row(request, "Apply playback preference", "Send these choices to the connected device", host::applyPlayback).setEnabled(!host.working);
            PreferenceCategory system = category(screen, "Android settings");
            row(system, "Bluetooth settings", "Pair devices and manage Android's saved preferences", host::androidSettings);
            row(system, "How playback and DSP differ", "Android selection is a preference. DSP status reports the encoder's observed state.", () -> host.dialog("Playback and DSP", "Lossless selection does not prove that every packet is lossless. DSP status shows the actual encoder state when a fresh observation is available. Receiver bit-perfect playback is not verified by this app."));
        }
        private void tuning(PreferenceScreen screen) {
            String reason = host.availability.optString("reason", "These controls store requested additional delay. Applying a DSP image requires a supported firmware backend.");
            tips(screen, host.availability.optBoolean("runtime_apply_supported") ? "Requested additional delay" : "Saved values · not applied", reason);
            PreferenceCategory shared = category(screen, "Additional TTP");
            row(shared, "HQ and Lossless", "+" + host.requested("adaptive_hq_ttp_delta_ms") + " ms · Shared setting", () -> host.editDelay("adaptive_hq_ttp_delta_ms", "HQ and Lossless additional delay")).setEnabled(host.capability("adaptive_hq_ttp_delta_ms") != null && !host.working);
            row(shared, "Adaptive LL", "+" + host.requested("adaptive_ll_ttp_delta_ms") + " ms · Independent setting", () -> host.editDelay("adaptive_ll_ttp_delta_ms", "Adaptive LL additional delay")).setEnabled(host.capability("adaptive_ll_ttp_delta_ms") != null && !host.working);
            PreferenceCategory explain = category(screen, "About delay");
            row(explain, "Additional delay is not total latency", "TTP is part of the encoder timing path. App, transport and earbud buffering also contribute to listening latency.", null);
            row(explain, "Inspect control capabilities", "Ranges, supported contexts and firmware providers", () -> host.dialog("Control capabilities", pretty(host.controls)));
        }
        private void dsp(PreferenceScreen screen) {
            JSONObject dsp = host.snapshot.optJSONObject("dsp"); if (dsp == null) dsp = new JSONObject();
            boolean available = dsp.optBoolean("available");
            tips(screen, available ? "Live encoder observations" : "No fresh DSP observation", available ? "Fields are checked against the current route and process session." : dsp.optString("reason", "Start a short diagnostic capture while audio is playing."));
            JSONObject loader = dsp.optJSONObject("loader");
            if (loader != null) row(screen, "DSP loader", loader.optString("message", loader.optString("code", "Unknown")), () -> host.dialog("DSP loader", pretty(host.snapshot.optJSONObject("dsp"))));
            PreferenceCategory observation = category(screen, "Encoder");
            row(observation, "Selected playback", host.codecSummary(), null);
            row(observation, "Receiver bit-perfect", "Unverified", null);
            JSONObject fields = dsp.optJSONObject("fields");
            if (fields != null && available) {
                String[] priority = {"encoder_state", "encoder_lossless", "calculated_ttp_us", "hq_target_us", "latency_state", "system_mode", "sample_rate_hz", "qhs", "rssi_dbm", "feedback_quality_level"};
                List<String> keys = new ArrayList<>();
                for (String key : priority) if (fields.has(key)) keys.add(key);
                for (Iterator<String> all = fields.keys(); all.hasNext();) { String key = all.next(); if (!keys.contains(key)) keys.add(key); }
                for (String key : keys) {
                    JSONObject field = fields.optJSONObject(key); if (field == null) continue;
                    String value = field.isNull("value") ? "Unknown" : String.valueOf(field.opt("value"));
                    boolean fresh = field.optBoolean("fresh");
                    String summary = value + " " + field.optString("unit", "") + " · " + (fresh ? "Current" : "Stale");
                    row(observation, key.replace('_', ' '), summary, () -> host.dialog(key.replace('_', ' '), pretty(field)));
                }
            } else row(observation, "Encoder mode, TTP and QHS", "Available during a fresh capture when the DSP emits these fields", null);
            PreferenceCategory inspect = category(screen, "Inspect");
            row(inspect, "Start a 30-second capture", host.capturing ? "A capture is already running" : "Collect a short snapshot of the current session", () -> host.capture(30)).setEnabled(!host.capturing);
            row(inspect, "Copy current status", "Structured JSON with field freshness and sources", () -> host.copy("Shizuka status", pretty(host.snapshot)));
        }
        private void diagnostics(PreferenceScreen screen) {
            tips(screen, host.capturing ? "Capturing DSP logs" : "Diagnostics on demand", "Capture runs only when you start it. Play audio through the connected Bluetooth device to observe its encoder.");
            PreferenceCategory capture = category(screen, "Capture");
            row(capture, "Quick capture", "15 seconds · check current DSP behavior", () -> host.capture(15)).setEnabled(!host.capturing);
            row(capture, "Standard capture", "30 seconds · encoder state and timing", () -> host.capture(30)).setEnabled(!host.capturing);
            row(capture, "Extended capture", "60 seconds · intermittent behavior", () -> host.capture(60)).setEnabled(!host.capturing);
            PreferenceCategory saved = category(screen, "Saved results");
            row(saved, "Saved captures", "Save a completed session as a ZIP to a location you choose", host::sessions).setEnabled(!host.working);
            row(saved, "Copy current status", "JSON snapshot with Android selection and DSP observations", () -> host.copy("Shizuka status", pretty(host.snapshot)));
            row(saved, "Last command output", "Inspect or copy the most recent module response", () -> host.dialog("Last command output", host.api.lastOutput));
        }
        private void module(PreferenceScreen screen) {
            tips(screen, "KernelSU module session", host.error == null && host.updatedAt > 0 ? "Connected to the temporary root session opened by KernelSU Action." : "Open the Shizuka module Action in KernelSU to start a temporary root session.");
            PreferenceCategory status = category(screen, "Module tools");
            row(status, "Check module status", "Verify active native components and trial state", () -> host.perform("Checking module status", () -> host.api.call("control", 30, "status")));
            row(status, "Activate or repair module", "Temporarily restarts Bluetooth and audio", () -> new AlertDialog.Builder(host).setTitle("Activate module?").setMessage("Bluetooth and audio will briefly disconnect. Current playback will be interrupted.").setNegativeButton("Cancel", null).setPositiveButton("Activate", (d,w) -> host.perform("Activating module", () -> host.api.call("apply", 180))).show());
            row(status, "Restore stock session", "Undo the running native experiment", () -> new AlertDialog.Builder(host).setTitle("Restore stock session?").setMessage("Bluetooth and audio will briefly disconnect. Saved delay requests are kept.").setNegativeButton("Cancel", null).setPositiveButton("Restore", (d,w) -> host.perform("Restoring stock session", () -> host.api.call("control", 180, "rollback"))).show());
            row(status, "Bluetooth settings", "Manage paired devices in Android", host::androidSettings);
        }
        private void about(PreferenceScreen screen) {
            tips(screen, "Shizuka", "Bluetooth playback, DSP observations and practical debugging in one place.");
            PreferenceCategory built = category(screen, "Built with OneUIProject");
            row(built, "One UI Design", "OneUIProject design 1.2.6 with SESL core components · MIT License", () -> host.dialog("OneUIProject", "This app uses OneUIProject's native ToolbarLayout, OneUITheme, SESL AppCompat, preferences and Material components.\n\nhttps://github.com/OneUIProject/oneui-design\nhttps://github.com/OneUIProject/oneui-core\n\nCopyright (c) 2022 Yanndroid & BlackMesa123. Licensed under the MIT License. The complete license is bundled in this app and its source."));
            row(built, "Open source licenses", "OneUIProject MIT License", () -> {
                try (java.io.InputStream source = host.getAssets().open("oneui-project-license.txt")) {
                    java.io.ByteArrayOutputStream data = new java.io.ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int length;
                    while ((length = source.read(buffer)) != -1) data.write(buffer, 0, length);
                    host.dialog("OneUIProject MIT License", new String(data.toByteArray(), java.nio.charset.StandardCharsets.UTF_8));
                } catch (Exception error) { host.toast("License text is unavailable."); }
            });
            row(built, "Settings app version", "0.10.0-test", null);
            row(built, "Observation model", "Saved requests, Android selection and DSP observations are separate states.", null);
        }
    }
}
