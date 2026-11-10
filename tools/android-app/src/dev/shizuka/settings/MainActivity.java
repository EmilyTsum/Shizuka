// SPDX-License-Identifier: GPL-2.0-only
package dev.shizuka.settings;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.res.TypedArray;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
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
import androidx.preference.PreferenceViewHolder;
import dev.oneuiproject.oneui.layout.ToolbarLayout;
import dev.oneuiproject.oneui.preference.TipsCardPreference;
import dev.oneuiproject.oneui.preference.LayoutPreference;
import androidx.appcompat.widget.AppCompatButton;
import android.graphics.drawable.GradientDrawable;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.text.DateFormat;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** A native OneUIProject settings surface over the module's shared APIs. */
public final class MainActivity extends AppCompatActivity {
    private final RootApi api = new RootApi();
    private final ExecutorService workers = Executors.newFixedThreadPool(2);
    private final ExecutorService connectionWorker = Executors.newSingleThreadExecutor();
    private final ExecutorService delayOperations = Executors.newSingleThreadExecutor();
    private final ExecutorService observer = Executors.newSingleThreadExecutor();
    private final ExecutorService observerLease = Executors.newSingleThreadExecutor();
    private final DspObservationSession observation = new DspObservationSession();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private JSONObject snapshot = new JSONObject(), controls = new JSONObject();
    private JSONObject playback = new JSONObject(), availability = new JSONObject();
    private ToolbarLayout toolbar;
    private SettingsFragment fragment;
    private String page = "home", error, errorKind, notice, archiveToSave;
    private String selectedFamily = "adaptive", selectedMode = "hq";
    private int selectedRate = 48000, selectedBits = 24;
    private boolean refreshing, working, capturing, resumed, connecting;
    private long updatedAt;
    private long statusReceivedElapsed;
    private long availabilityReceivedElapsed;
    private final LatencyApplyState.Requests delayRequests = new LatencyApplyState.Requests();
    private LatencyApplyState.Status delayStatus;
    private JSONObject delayResponse;
    private String delayOperationId, delayError;
    private boolean delaySubmitting, reconcileDelayLatest;
    private final Runnable delayExpiry = () -> {
        if (resumed && page.equals("tuning")) render();
    };
    private final Runnable dspExpiry = () -> { if (resumed) render(); };
    private String observationError;
    private String renderedDspOverview;
    private List<Object> renderedTuning;
    private boolean observationLeaseBusy;
    private final Runnable observeTick = new Runnable() {
        @Override public void run() {
            if (!resumed) return;
            syncObservation();
            ui.postDelayed(this, 1000);
        }
    };
    private interface Work { String run() throws Exception; }
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (!resumed) return;
            refresh(false);
            if (page.equals("tuning")) checkDelayOperation();
            ui.postDelayed(this, DspDisplayState.pollInterval(page));
        }
    };

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        api.bind(getIntent().getStringExtra("shizuka_socket"));
        String previousOperation = getPreferences(MODE_PRIVATE).getString("latency_operation_id", null);
        if (LatencyApplyState.uuid(previousOperation)) delayOperationId = previousOperation;
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
        toolbar.setNavigationButtonOnClickListener(view -> onBackPressed());
        render();
        if (api.hasSession()) refresh(true); else connectModule();
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent); setIntent(intent);
        api.bind(intent.getStringExtra("shizuka_socket"));
        resetDelaySession();
        error = null; errorKind = null; notice = null; refreshing = false; connecting = false; working = false;
        snapshot = new JSONObject(); controls = new JSONObject(); playback = new JSONObject();
        availability = new JSONObject(); availabilityReceivedElapsed = 0; updatedAt = 0; syncObservation(); render();
        if (api.hasSession()) refresh(true); else connectModule();
    }
    @Override protected void onResume() {
        super.onResume(); resumed = true; render(); ui.post(poll); ui.post(observeTick);
    }
    @Override protected void onPause() { resumed = false; ui.removeCallbacks(poll); ui.removeCallbacks(observeTick); ui.removeCallbacks(dspExpiry); syncObservation(); super.onPause(); }
    @Override protected void onDestroy() {
        resumed = false; ui.removeCallbacks(poll); ui.removeCallbacks(observeTick); syncObservation();
        ui.removeCallbacks(delayExpiry); ui.removeCallbacks(dspExpiry); delayRequests.reset();
        api.cancelBootstrap(); connectionWorker.shutdown(); workers.shutdown(); delayOperations.shutdown(); observer.shutdown(); observerLease.shutdown(); super.onDestroy();
    }
    @Override protected void onSaveInstanceState(Bundle out) { out.putString("page", page); super.onSaveInstanceState(out); }
    @Override public void onBackPressed() { if (page.equals("dsp_details")) show("dsp"); else if (!page.equals("home")) show("home"); else super.onBackPressed(); }
    @Override public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, 1, 0, "Refresh").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        menu.add(0, 2, 1, "Copy status").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        return true;
    }
    @Override public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == 1) {
            if (error != null || !api.hasSession()) connectModule();
            else { refresh(true); if (page.equals("tuning")) checkDelayOperation(); }
            return true;
        }
        if (item.getItemId() == 2) { copy("Shizuka status", pretty(snapshot)); return true; }
        return super.onOptionsItemSelected(item);
    }
    private void connectModule() {
        if (connecting || isDestroyed() || isFinishing()) return;
        api.bind(null);
        final long connectionGeneration = api.generation();
        connecting = true; refreshing = false; working = false; error = null; errorKind = null; notice = null;
        snapshot = new JSONObject(); controls = new JSONObject(); playback = new JSONObject();
        availability = new JSONObject(); updatedAt = 0; statusReceivedElapsed = 0; availabilityReceivedElapsed = 0;
        resetDelaySession(); syncObservation(); render();
        connectionWorker.execute(() -> {
            String socket = null, failure = null, kind = null;
            try { socket = api.bootstrap(connectionGeneration); }
            catch (Exception problem) {
                failure = clean(problem);
                kind = problem instanceof RootApi.Unavailable ? ((RootApi.Unavailable)problem).kind : "connection";
            }
            final String opened = socket, message = failure, failedKind = kind;
            ui.post(() -> {
                if (isDestroyed() || connectionGeneration != api.generation()) return;
                connecting = false;
                if (message != null) { error = message; errorKind = failedKind; render(); return; }
                api.bind(opened); resetDelaySession(); render(); refresh(true);
            });
        });
    }
    private boolean dspPage() { return page.equals("dsp") || page.equals("dsp_details"); }
    private boolean encoderCodec() { int type = codec().optInt("type", -1); return type == 7 || type == 12; }
    private String observationHint() {
        if (!codec().optBoolean("available")) return "Connect a Bluetooth audio device";
        if (!encoderCodec()) return "ENCLEVEL is unavailable for the selected codec";
        JSONObject route = snapshot.optJSONObject("route");
        if (route == null || !"A2DP".equals(route.optString("transport"))) return "Waiting for Bluetooth audio";
        if (!device().optBoolean("stream_started")) return "Play audio to see the encoder state";
        JSONObject dsp = snapshot.optJSONObject("dsp");
        JSONObject bitrate = currentDspField(dsp, "feedback_bitrate_level");
        if (bitrate != null) return "Bitrate level " + bitrate.optString("value") + " · live feedback";
        if (observationError != null) return "Observation temporarily unavailable · See details";
        return "Listening for encoder updates…";
    }
    private void show(String next) {
        page = next; notice = null; syncObservation(); render();
        if (next.equals("home")) toolbar.setExpanded(true, false);
        refresh(next.equals("playback") || next.equals("tuning"));
        if (next.equals("tuning")) checkDelayOperation();
        if (resumed) { ui.removeCallbacks(poll); ui.postDelayed(poll, DspDisplayState.pollInterval(page)); }
    }
    private void syncObservation() {
        JSONObject route = snapshot.optJSONObject("route");
        JSONArray devices = snapshot.optJSONArray("devices");
        long elapsed = SystemClock.elapsedRealtime() - statusReceivedElapsed;
        boolean eligible = resumed && dspPage() && !capturing && error == null && updatedAt > 0 &&
            statusReceivedElapsed > 0 && elapsed >= 0 && elapsed < 4000 && encoderCodec() &&
            devices != null && devices.length() == 1 && device().optBoolean("stream_started") &&
            route != null && "A2DP".equals(route.optString("transport"));
        String context = api.generation() + ":" + connectionName() + ":" + codec().toString();
        DspObservationSession.Action action = observation.update(eligible, context, SystemClock.elapsedRealtime());
        if (action.stop != null) {
            observationError = null;
            observerLease.execute(() -> {
                try { api.json("diagnostics", 4, "stop", action.stop); } catch (Exception ignored) { /* The five-second lease is the independent cleanup bound. */ }
            });
        }
        if (action.start != null) {
            observationError = null;
            final String token = action.start;
            observer.execute(() -> {
                if (!observation.owns(token)) return;
                observation.started(token, SystemClock.elapsedRealtime());
                String failed = null;
                try { api.json("diagnostics", 155, "observe", token); }
                catch (Exception problem) { failed = clean(problem); }
                final String failure = failed;
                ui.post(() -> {
                    if (observation.completed(token, failure != null, SystemClock.elapsedRealtime())) {
                        observationError = failure;
                        if (!isDestroyed()) { render(); if (resumed) refresh(false); }
                    }
                });
            });
        }
        if (action.renew != null && !observationLeaseBusy) {
            observationLeaseBusy = true;
            observerLease.execute(() -> {
                String failed = null;
                try {
                    JSONObject result = api.json("diagnostics", 4, "renew", action.renew);
                    if (!result.optBoolean("running")) failed = "The observation lease ended";
                } catch (Exception problem) { failed = clean(problem); }
                final String failure = failed;
                ui.post(() -> {
                    observationLeaseBusy = false;
                    if (failure != null && observation.completed(action.renew, true, SystemClock.elapsedRealtime())) {
                        observationError = failure;
                        if (!isDestroyed()) render();
                    }
                });
            });
        }
    }
    private void render() {
        if (isFinishing() || isDestroyed() || toolbar == null) return;
        // Each field can expire at a different time. Rearm before the unchanged
        // overview return so a blocked status request cannot leave later values live.
        scheduleDspExpiry();
        if (page.equals("dsp")) {
            String overview = dspOverviewKey();
            if (overview.equals(renderedDspOverview)) return;
            renderedDspOverview = overview;
        } else renderedDspOverview = null;
        if (page.equals("tuning")) {
            LatencyApplyState.View operation = delayView();
            // Renew the expiry timer even when only the latest receipt changed.
            // A skipped screen rebuild must never retain an expired proof/action.
            scheduleDelayExpiry(operation);
            List<Object> visible = tuningDisplayKey(operation);
            if (visible.equals(renderedTuning)) return;
            renderedTuning = visible;
        } else { renderedTuning = null; ui.removeCallbacks(delayExpiry); }
        String title = page.equals("home") ? "Shizuka" : page.equals("playback") ? "Playback"
            : page.equals("tuning") ? "Additional delay" : page.equals("dsp") ? "DSP status" : page.equals("dsp_details") ? "DSP details"
            : page.equals("diagnostics") ? "Diagnostics" : page.equals("module") ? "Module" : "About Shizuka";
        toolbar.setTitle(title);
        toolbar.setExpandedSubtitle(page.equals("home") ? "Your Bluetooth workspace" : null);
        toolbar.setNavigationButtonVisible(!page.equals("home"));
        toolbar.setExpandable(page.equals("home"));
        if (fragment != null && fragment.isAdded()) fragment.rebuild();
    }
    private void scheduleDspExpiry() {
        ui.removeCallbacks(dspExpiry);
        if (!resumed || !(page.equals("home") || dspPage())) return;
        JSONObject dsp = snapshot.optJSONObject("dsp");
        if (dsp == null || !dsp.optBoolean("available")) return;
        List<Double> ages = new ArrayList<>();
        JSONObject fields = dsp.optJSONObject("fields");
        if (fields != null) for (Iterator<String> keys = fields.keys(); keys.hasNext();) {
            JSONObject field = fields.optJSONObject(keys.next());
            if (field != null && field.optBoolean("fresh") && !"encoder_epoch".equals(field.optString("scope")))
                ages.add(field.optDouble("age_seconds", -1));
        }
        long delay = DspDisplayState.nextExpiryDelay(dsp.optDouble("age_seconds", -1), ages,
            statusReceivedElapsed, SystemClock.elapsedRealtime());
        if (delay > 0) ui.postDelayed(dspExpiry, delay);
    }
    private List<Object> tuningDisplayKey(LatencyApplyState.View operation) {
        TuningDisplayState.Input visible = new TuningDisplayState.Input();
        visible.ready = updatedAt > 0; visible.error = error; visible.errorKind = errorKind; visible.notice = notice;
        visible.working = working; visible.capturing = capturing;
        visible.hqMs = requested("adaptive_hq_ttp_delta_ms"); visible.llMs = requested("adaptive_ll_ttp_delta_ms");
        visible.hqControl = capability("adaptive_hq_ttp_delta_ms") != null;
        visible.llControl = capability("adaptive_ll_ttp_delta_ms") != null;
        visible.operationId = delayOperationId; visible.operationError = delayError; visible.operation = operation;
        visible.runtimeFresh = runtimeObservationFresh(); visible.runtimeApplied = runtimeDelayApplied();
        visible.runtime = jsonValues(availability);
        return TuningDisplayState.key(visible);
    }
    private String dspOverviewKey() {
        JSONObject dsp = snapshot.optJSONObject("dsp");
        String state = currentEncoderState(dsp);
        JSONObject history = dsp == null ? null : dsp.optJSONObject("history");
        JSONObject saved = history == null ? null : history.optJSONObject("fields");
        JSONArray changes = dsp == null ? null : dsp.optJSONArray("encoder_transitions");
        JSONObject change = changes == null || changes.length() == 0 ? null : changes.optJSONObject(changes.length() - 1);
        return object("ready", updatedAt > 0, "error", error, "notice", notice, "state", state,
            "quality", fieldValue(currentDspField(dsp, "quality_level")),
            "bitrate", fieldValue(currentDspField(dsp, "feedback_bitrate_level")),
            "frame_bitrate", dspBitrateKey(dsp),
            "playback", codecSummary(), "hint", observationHint(),
            "change", state != null && change != null ? transitionTime(dsp, change) + change.optString("previous") + change.optString("state") : "",
            "history", state == null && saved != null && (saved.has("encoder_state") || saved.has("quality_level"))
                ? fieldValue(saved.optJSONObject("encoder_state")) + fieldValue(saved.optJSONObject("quality_level")) + captureTime(history) + history.optBoolean("same_session") : "").toString();
    }
    private String dspBitrateKey(JSONObject dsp) {
        JSONObject fields = dsp == null ? null : dsp.optJSONObject("fields");
        JSONObject frame = fields == null ? null : fields.optJSONObject("encoder_frame_bitrate_bps");
        return DspDisplayState.bitrateKey(frame == null ? null : frame.opt("value"),
            frame != null && frame.optBoolean("fresh"), frame == null ? -1 : frame.optDouble("age_seconds", -1),
            statusReceivedElapsed, SystemClock.elapsedRealtime(), dspCurrent(dsp));
    }
    private static String fieldValue(JSONObject field) { return field == null ? "" : field.optString("value"); }
    private static String bitrateLabel(JSONObject field) {
        return field == null ? "Not available from the current logs" : String.format(Locale.US, "%,.1f kb/s", field.optDouble("value") / 1000.0);
    }
    private static String dspFieldLabel(String key) {
        switch (key) {
        case "encoder_frame_bitrate_bps": return "Encoder bitrate (frame estimate)";
        case "packet_payload_bytes": return "Frame size per channel";
        case "encoder_output_buffer_bytes": return "Encoder output buffer";
        case "packet_interval_ms": return "Frame interval";
        case "desired_encoder_bitrate_raw": return "Requested encoder bitrate (raw)";
        case "feedback_bitrate_level": return "Bitrate level (feedback)";
        default: return key.replace('_', ' ');
        }
    }
    private void refresh(boolean full) {
        if (connecting || !api.hasSession() || refreshing || isFinishing() || isDestroyed()) return;
        refreshing = true;
        final long requestGeneration = api.generation();
        workers.execute(() -> {
            JSONObject fresh = null, caps = null, playing = null, possible = null;
            long statusReceipt = 0, availabilityReceipt = 0;
            String failed = null, failedKind = null;
            try {
                fresh = api.json("status", 20);
                statusReceipt = SystemClock.elapsedRealtime();
                if (full || controls.length() == 0) {
                    caps = api.json("config", 20, "tuning", "capabilities");
                }
                if (full || page.equals("tuning")) {
                    try { possible = api.json("config", 20, "tuning", "availability"); }
                    catch (Exception unavailable) { possible = object("available", false, "applied", false, "reason", clean(unavailable)); }
                    availabilityReceipt = SystemClock.elapsedRealtime();
                }
                if (page.equals("playback") && full) {
                    try { playing = api.json("playback", 20, "get", "active"); }
                    catch (Exception e) { playing = object("available", false, "error", clean(e)); }
                }
            } catch (Exception e) { failed = clean(e); failedKind = e instanceof RootApi.Unavailable ? ((RootApi.Unavailable)e).kind : "connection"; }
            final JSONObject a = fresh, b = caps, c = playing, d = possible;
            final long receivedStatus = statusReceipt, receivedAvailability = availabilityReceipt;
            final String message = failed, kind = failedKind;
            ui.post(() -> {
                if (isDestroyed() || requestGeneration != api.generation()) return;
                refreshing = false; error = message; errorKind = kind;
                if (message != null) {
                    snapshot = new JSONObject(); controls = new JSONObject(); playback = new JSONObject();
                    availability = new JSONObject(); availabilityReceivedElapsed = 0; updatedAt = 0; page = "home";
                    resetDelaySession(); syncObservation(); render(); return;
                }
                if (a != null) {
                    snapshot = a; updatedAt = System.currentTimeMillis();
                    statusReceivedElapsed = receivedStatus;
                }
                if (b != null) controls = b;
                if (d != null) {
                    availability = d;
                    availabilityReceivedElapsed = receivedAvailability;
                    if (d.optBoolean("available")) {
                        final long receipt = availabilityReceivedElapsed;
                        long remaining = Math.min(2000 - d.optLong("observation_age_ms", 2000), d.optLong("lease_remaining_ms"));
                        remaining -= SystemClock.elapsedRealtime() - receipt;
                        ui.postDelayed(() -> {
                            if (!isDestroyed() && availabilityReceivedElapsed == receipt) render();
                        }, Math.max(1, remaining));
                    }
                }
                if (c != null) { playback = c; readSelectedPlayback(); }
                syncObservation(); render();
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
        final long requestGeneration = api.generation();
        workers.execute(() -> {
            if (requestGeneration != api.generation()) return;
            String message;
            try { message = work.run(); }
            catch (Exception e) { message = clean(e); }
            final String result = message;
            ui.post(() -> { if (isDestroyed() || requestGeneration != api.generation()) return; working = false; notice = result; render(); refresh(true); });
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
    private boolean dspCurrent(JSONObject dsp) {
        if (dsp == null || !dsp.optBoolean("available") || statusReceivedElapsed == 0) return false;
        return ObservationFreshness.current(dsp.optDouble("age_seconds", -1), statusReceivedElapsed, SystemClock.elapsedRealtime(), 3000);
    }
    private JSONObject currentDspField(JSONObject dsp, String key) {
        JSONObject fields = dsp == null ? null : dsp.optJSONObject("fields");
        JSONObject field = fields == null ? null : fields.optJSONObject(key);
        if (!dspCurrent(dsp) || field == null || !field.optBoolean("fresh") || field.isNull("value")) return null;
        double age = field.optDouble("age_seconds", -1);
        boolean epoch = key.equals("sample_rate_hz") && field.optString("scope").equals("encoder_epoch");
        return Double.isFinite(age) && age >= 0 && (epoch || ObservationFreshness.current(age, statusReceivedElapsed, SystemClock.elapsedRealtime(), 3000)) ? field : null;
    }
    private String currentEncoderState(JSONObject dsp) {
        JSONObject field = currentDspField(dsp, "encoder_state");
        String state = field == null ? "" : field.optString("value");
        return state.matches("STATE_ENCLEVEL_[0-4]|STATE_ENCLEVEL_5_LOSSLESS") ? state : null;
    }
    private static String encoderLabel(String state) {
        if ("STATE_ENCLEVEL_5_LOSSLESS".equals(state)) return "Level 5 · Lossless";
        return state != null && state.matches("STATE_ENCLEVEL_[0-4]") ? "Level " + state.charAt(state.length() - 1) : "Unknown encoder state";
    }
    private static String captureTime(JSONObject history) {
        long saved = history == null ? 0 : history.optLong("observed_at_ms", history.optLong("saved_at_ms", 0));
        return saved > 0 ? DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT, Locale.US).format(new Date(saved))
            : "Capture time unavailable";
    }
    private static long transitionMillis(JSONObject container, JSONObject transition) {
        long at = transition.optLong("observed_at_ms", 0);
        if (at > 0) return at;
        long saved = container.optLong("collected_at_ms", container.optLong("saved_at_ms", 0));
        double age = transition.optDouble("age_seconds", -1);
        return saved > 0 && Double.isFinite(age) && age >= 0 && age * 1000 <= saved ? saved - (long)(age * 1000) : 0;
    }
    private static String transitionTime(JSONObject container, JSONObject transition) {
        long at = transitionMillis(container, transition);
        return at > 0 ? DateFormat.getTimeInstance(DateFormat.MEDIUM, Locale.US).format(new Date(at)) : "Time unavailable";
    }
    private String dspSummary() {
        JSONObject dsp = snapshot.optJSONObject("dsp");
        String state = currentEncoderState(dsp);
        if (state != null) return encoderLabel(state);
        JSONObject bitrate = currentDspField(dsp, "feedback_bitrate_level");
        if (bitrate != null) return "Bitrate level " + bitrate.optString("value") + " · live feedback";
        JSONObject history = dsp == null ? null : dsp.optJSONObject("history");
        JSONObject fields = history == null ? null : history.optJSONObject("fields");
        JSONObject previous = fields == null ? null : fields.optJSONObject("encoder_state");
        if (previous != null && !previous.isNull("value"))
            return "Last captured: " + encoderLabel(previous.optString("value")) + " · " + captureTime(history);
        return encoderCodec() ? "ENCLEVEL · Open for automatic updates" : "ENCLEVEL unavailable for this codec";
    }
    private boolean runtimeObservationFresh() {
        if (!availability.optBoolean("available") || !availability.optBoolean("fresh") ||
            !availability.optBoolean("lease_valid") || availabilityReceivedElapsed == 0) return false;
        long elapsed = SystemClock.elapsedRealtime() - availabilityReceivedElapsed;
        long age = availability.optLong("observation_age_ms", -1);
        long remaining = availability.optLong("lease_remaining_ms", 0);
        return elapsed >= 0 && age >= 0 && age + elapsed < 2000 && elapsed < remaining;
    }
    private boolean runtimeDelayApplied() {
        String epoch = availability.optString("graph_epoch", "0");
        long frames = availability.optLong("wr_frames", 0);
        return runtimeObservationFresh() && availability.optBoolean("applied") &&
            frames > 0 && availability.optLong("rd_frames", 0) >= frames &&
            (availability.optLong("active_delta_us", -1) == 0 ||
                availability.optLong("confirmed_rewritten_wrappers", 0) > 0) &&
            !epoch.equals("0") && !epoch.isEmpty() && !availability.isNull("active_delta_us");
    }
    private static String milliseconds(long us) {
        return us % 1000 == 0 ? (us / 1000) + " ms" : String.format(Locale.US, "%.3f ms", us / 1000.0);
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
                dialog.dismiss(); resetDelaySession(); perform("Saving requested delay", () -> {
                    api.json("config", 20, "tuning", "set", key, text);
                    return "Saved +" + value + " ms. The request is pending a confirmed new session.";
                });
            } catch (Exception invalid) { input.setError("Enter " + min + "–" + max + " in steps of " + step + " ms"); }
        }));
        dialog.show();
    }
    private void resetDelaySession() {
        delayRequests.reset(); delayStatus = null; delayResponse = null;
        delayError = null; delaySubmitting = false; reconcileDelayLatest = false; ui.removeCallbacks(delayExpiry);
        // Only the UUID survives a bridge change. Proof and capabilities never do.
    }
    private static Map<String, Object> jsonValues(JSONObject source) {
        Map<String, Object> result = new HashMap<>();
        for (Iterator<String> keys = source.keys(); keys.hasNext();) {
            String key = keys.next(); Object value = source.opt(key);
            result.put(key, value == JSONObject.NULL ? null : value);
        }
        return result;
    }
    private int savedDelay(String key, int maximum) {
        JSONObject tuning = snapshot.optJSONObject("tuning");
        JSONObject requested = tuning == null ? null : tuning.optJSONObject("requested");
        if (requested == null) return -1;
        long value = LatencyApplyState.number(jsonValues(requested), key, -1);
        return value >= 0 && value <= maximum ? (int) value : -1;
    }
    private LatencyApplyState.View delayView() {
        int hq = savedDelay("adaptive_hq_ttp_delta_ms", 150);
        int ll = savedDelay("adaptive_ll_ttp_delta_ms", 100);
        boolean usable = error == null && updatedAt > 0 && hq >= 0 && ll >= 0;
        LatencyApplyState.Expected expected = new LatencyApplyState.Expected(delayOperationId,
            Math.max(0, hq), Math.max(0, ll), api.generation());
        return LatencyApplyState.evaluate(expected, delayStatus, SystemClock.elapsedRealtime(), usable,
            delaySubmitting || working);
    }
    private void scheduleDelayExpiry(LatencyApplyState.View state) {
        ui.removeCallbacks(delayExpiry);
        long expiry = state.proofExpiryDelayMs;
        if (state.canApply && delayStatus != null) {
            long capabilityExpiry = 2001 - (SystemClock.elapsedRealtime() - delayStatus.receivedElapsedMs);
            expiry = expiry > 0 ? Math.min(expiry, capabilityExpiry) : capabilityExpiry;
        }
        if (expiry > 0) ui.postDelayed(delayExpiry, expiry);
    }
    private void checkDelayOperation() {
        if (!resumed || !page.equals("tuning") || error != null || updatedAt == 0 || isDestroyed()) return;
        LatencyApplyState.Requests.Ticket request = delayRequests.begin(api.generation());
        if (request == null) return;
        callDelayOperation(request, reconcileDelayLatest ? null : delayOperationId, -1, -1);
    }
    private void applySavedDelay() {
        LatencyApplyState.View state = delayView();
        if (!resumed || !page.equals("tuning") || !state.canApply) { toast(state.detail); return; }
        LatencyApplyState.Requests.Ticket request = delayRequests.begin(api.generation());
        if (request == null) { toast("Checking the current operation. Please wait."); return; }
        int hq = savedDelay("adaptive_hq_ttp_delta_ms", 150);
        int ll = savedDelay("adaptive_ll_ttp_delta_ms", 100);
        delayOperationId = UUID.randomUUID().toString(); reconcileDelayLatest = false;
        delaySubmitting = true; delayStatus = null; delayResponse = null; delayError = null;
        render();
        callDelayOperation(request, delayOperationId, hq, ll);
    }
    private void callDelayOperation(LatencyApplyState.Requests.Ticket request, String id, int hq, int ll) {
        final boolean launch = hq >= 0;
        delayOperations.execute(() -> {
            JSONObject result = null;
            LatencyApplyState.Status status = null;
            String failure = null;
            try {
                if (!delayRequests.current(request, api.generation())) return;
                // Persist the UUID before launch. Never persist accepted/applied or a proof receipt.
                if (launch && !getPreferences(MODE_PRIVATE).edit().putString("latency_operation_id", id).commit())
                    throw new IllegalStateException("The operation identity could not be saved. No request was sent.");
                if (!delayRequests.current(request, api.generation())) return;
                result = launch ? api.json("control", 20, "latency-apply", id, Integer.toString(hq), Integer.toString(ll))
                    : id == null ? api.json("control", 20, "latency-apply-status")
                    : api.json("control", 20, "latency-apply-status", id);
                long receipt = SystemClock.elapsedRealtime();
                status = LatencyApplyState.decode(jsonValues(result), request.generation, receipt);
            } catch (Exception problem) { failure = clean(problem); }
            final JSONObject response = result;
            final LatencyApplyState.Status received = status;
            final String problem = failure;
            ui.post(() -> {
                if (isDestroyed() || !delayRequests.finish(request, api.generation())) return;
                delaySubmitting = false; delayStatus = received; delayResponse = response; delayError = problem;
                if (id == null && LatencyApplyState.reconcileLatest(received, api.generation(), SystemClock.elapsedRealtime())) {
                    delayOperationId = received.operationId; reconcileDelayLatest = false;
                    if (delayOperationId == null) getPreferences(MODE_PRIVATE).edit().remove("latency_operation_id").apply();
                    else getPreferences(MODE_PRIVATE).edit().putString("latency_operation_id", delayOperationId).apply();
                } else if (id != null && received != null && received.schema == 1 &&
                    "latency_application".equals(received.scope) && id.equals(received.operationId) &&
                    received.operationKnownReported && ((!received.operationKnown && received.terminal) ||
                        (received.currentOperationReported && LatencyApplyState.uuid(received.currentOperationId) &&
                            !id.equals(received.currentOperationId)))) {
                    // Missing history or a newer owner cannot release ownership. Ask the current owner separately.
                    reconcileDelayLatest = true;
                }
                if (resumed && page.equals("tuning")) render();
                // A lost launch response is resolved by polling this UUID, never by relaunching.
                if (launch && resumed && page.equals("tuning")) checkDelayOperation();
            });
        });
    }
    private void delayOperationDetails() {
        String identity = delayOperationId == null ? "No operation has been recorded." : "Operation: " + delayOperationId;
        String failure = delayError == null ? "" : "\n\nStatus request: " + delayError;
        dialog("Delay operation details", identity + failure + (delayResponse == null ? "" : "\n\n" + pretty(delayResponse)));
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
        syncObservation();
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
            getListView().seslSetLastRoundedCorner(true);
            int bottom = Math.round(16 * getResources().getDisplayMetrics().density);
            getListView().setPadding(getListView().getPaddingLeft(), getListView().getPaddingTop(),
                getListView().getPaddingRight(), bottom);
            getListView().setClipToPadding(false);
        }
        private PreferenceCategory category(PreferenceScreen screen, String title) {
            PreferenceCategory category = new PreferenceCategory(requireContext()); category.setTitle(title); screen.addPreference(category); return category;
        }
        private Preference row(PreferenceGroup group, String title, String summary, Runnable action) {
            Preference pref = new Preference(requireContext()) {
                @Override public void onBindViewHolder(PreferenceViewHolder holder) {
                    super.onBindViewHolder(holder);
                    int touch = Math.round(48 * getResources().getDisplayMetrics().density);
                    holder.itemView.setMinimumHeight(Math.max(touch, holder.itemView.getMinimumHeight()));
                    if (action == null) {
                        // SESL copies summary grey onto enabled, non-selectable
                        // titles. Keep informational values readable and inert.
                        holder.itemView.setEnabled(true);
                        holder.itemView.setOnClickListener(null);
                        holder.itemView.setClickable(false);
                        TypedArray colors = getContext().obtainStyledAttributes(new int[] {android.R.attr.textColorPrimary});
                        ColorStateList primary = colors.getColorStateList(0); colors.recycle();
                        for (int id : new int[] {android.R.id.title, android.R.id.summary}) {
                            View value = holder.findViewById(id);
                            if (value instanceof TextView) {
                                value.setEnabled(true);
                                if (primary != null) ((TextView)value).setTextColor(primary);
                            }
                        }
                    }
                }
            };
            pref.setTitle(title); pref.setSummary(summary); pref.setPersistent(false); pref.setIconSpaceReserved(false);
            if (action != null) pref.setOnPreferenceClickListener(p -> { action.run(); return true; });
            else { pref.setEnabled(true); pref.setShouldDisableView(false); pref.setSelectable(false); pref.setOnPreferenceClickListener(null); }
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
                tips(screen, "root".equals(host.errorKind) ? "Root access needed"
                    : "session".equals(host.errorKind) ? "Module session unavailable" : "Shizuka module unavailable", host.error);
                row(screen, "Retry connection", "Request root access and reconnect to the installed module", host::connectModule);
                row(screen, "Connection details", "Inspect the last connection or module response", () -> host.dialog("Connection details", host.api.lastOutput));
                row(screen, "Close", "Close Shizuka", host::finish);
                return;
            }
            if (host.updatedAt == 0) {
                tips(screen, host.connecting ? "Connecting to Shizuka" : "Opening module session",
                    host.connecting ? "Allow root access in KernelSU if requested. Your audio settings are kept." : "Reading the installed module session.");
                row(screen, "Close", "Close Shizuka", host::finish);
                return;
            }
            if (host.notice != null) tips(screen, host.working || host.capturing ? "In progress" : "Latest action", host.notice);
            switch (host.page) {
            case "home": home(screen); break;
            case "playback": playback(screen); break;
            case "tuning": tuning(screen); break;
            case "dsp": dsp(screen); break;
            case "dsp_details": dspDetails(screen); break;
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
            row(inspect, "DSP status", host.dspSummary(), () -> host.show("dsp"));
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
            boolean fresh = host.runtimeObservationFresh(), applied = host.runtimeDelayApplied();
            LatencyApplyState.View operation = host.delayView();
            tips(screen, operation.title, operation.detail);
            PreferenceCategory shared = category(screen, "Saved additional delay");
            row(shared, "HQ and Lossless", "+" + host.requested("adaptive_hq_ttp_delta_ms") + " ms · Shared setting", () -> host.editDelay("adaptive_hq_ttp_delta_ms", "HQ and Lossless additional delay")).setEnabled(host.capability("adaptive_hq_ttp_delta_ms") != null && !host.working);
            row(shared, "Adaptive LL", "+" + host.requested("adaptive_ll_ttp_delta_ms") + " ms · Independent setting", () -> host.editDelay("adaptive_ll_ttp_delta_ms", "Adaptive LL additional delay")).setEnabled(host.capability("adaptive_ll_ttp_delta_ms") != null && !host.working);
            LinearLayout actions = new LinearLayout(requireContext());
            actions.setOrientation(LinearLayout.VERTICAL);
            int inset = Math.round(24 * getResources().getDisplayMetrics().density);
            actions.setPadding(inset, inset / 2, inset, inset / 2);
            AppCompatButton apply = new AppCompatButton(requireContext());
            apply.setText(LatencyApplyState.BUTTON);
            TypedArray buttonColors = requireContext().obtainStyledAttributes(new int[] {android.R.attr.colorButtonNormal});
            ColorStateList face = buttonColors.getColorStateList(0); buttonColors.recycle();
            GradientDrawable shape = new GradientDrawable(); shape.setCornerRadius(inset);
            shape.setColor(face == null ? ColorStateList.valueOf(0xffe0e0e0) : face);
            apply.setSupportBackgroundTintList(null); apply.setBackground(shape);
            apply.setMinHeight(Math.round(48 * getResources().getDisplayMetrics().density));
            apply.setEnabled(operation.canApply);
            apply.setOnClickListener(view -> host.applySavedDelay());
            actions.addView(apply, new LinearLayout.LayoutParams(-1, -2));
            LayoutPreference action = new LayoutPreference(requireContext(), actions);
            action.setSelectable(false); action.setPersistent(false);
            action.setAllowDividerAbove(false); action.setAllowDividerBelow(false);
            screen.addPreference(action);
            row(screen, "Operation details", host.delayError != null ? "Status unavailable · Tap for details"
                : host.delayOperationId == null ? "Capabilities and application status" : "Current operation and proof details",
                host::delayOperationDetails);
            PreferenceCategory observed = category(screen, "Packet observation");
            row(observed, "Current graph", applied
                ? "+" + milliseconds(host.availability.optLong("active_delta_us")) + " · R3 HQ · 44.1 kHz"
                : "No confirmed active additional delay", null);
            boolean pending = !applied || host.requested("adaptive_hq_ttp_delta_ms") * 1000L != host.availability.optLong("active_delta_us", -1);
            row(observed, "HQ request", pending
                ? "+" + host.requested("adaptive_hq_ttp_delta_ms") + " ms · Pending the next confirmed session"
                : "Matches the current graph", null);
            row(observed, "Adaptive LL observation", "No active LL transform is verified", null);
            row(observed, "Packet TTP", applied
                ? milliseconds(host.availability.optLong("last_before_ttp_us")) + " → " + milliseconds(host.availability.optLong("last_after_ttp_us"))
                : "No current completed-buffer timing observation", null);
            row(observed, "Completed buffers", fresh
                ? "RD " + host.availability.optString("rd_frames", "0") + " · WR " + host.availability.optString("wr_frames", "0")
                : "No current runtime counters", null);
            row(observed, "Buffer ownership", fresh
                ? "Mapped " + host.availability.optString("owned_mappings", "0") +
                    " · In flight " + host.availability.optString("in_flight_buffers", "0") +
                    " · Confirmed rewrites " + host.availability.optString("confirmed_rewritten_wrappers", "0")
                : "No current ownership observation", null);
            row(observed, "Runtime details", fresh
                ? "Graph " + host.availability.optString("graph_epoch", "0") + " · Stage " + host.availability.optInt("stage") +
                    " · Error " + host.availability.optInt("error")
                : "View the latest availability response", () -> host.dialog("Runtime observation", pretty(host.availability)));
            PreferenceCategory explain = category(screen, "About delay");
            row(explain, "Additional delay is not total latency", "TTP is part of the encoder timing path. App, transport and earbud buffering also contribute to listening latency.", null);
            row(explain, "Receiver timing", "Completed relay buffers confirm the packet timing change. Listening latency and receiver buffering need separate measurement.", null);
            row(explain, "Inspect control capabilities", "Ranges, supported contexts and firmware providers", () -> host.dialog("Control capabilities", pretty(host.controls)));
        }
        private void dsp(PreferenceScreen screen) {
            JSONObject dsp = host.snapshot.optJSONObject("dsp"); if (dsp == null) dsp = new JSONObject();
            String state = host.currentEncoderState(dsp);
            tips(screen, state != null ? "Live encoder" : "Encoder status", state != null ? encoderLabel(state) : host.observationHint());
            PreferenceCategory observation = category(screen, "Encoder");
            JSONObject quality = host.currentDspField(dsp, "quality_level");
            if (quality != null) row(observation, "Encoder quality", "qlevel " + quality.optString("value"), null);
            JSONObject bitrate = host.currentDspField(dsp, "feedback_bitrate_level");
            if (bitrate != null) row(observation, "Bitrate level", bitrate.optString("value") + " · Feedback", null);
            row(observation, "Encoder bitrate (frame estimate)", bitrateLabel(host.currentDspField(dsp, "encoder_frame_bitrate_bps")), null);
            JSONArray changes = dsp.optJSONArray("encoder_transitions");
            if (state != null && changes != null && changes.length() > 0) {
                JSONObject change = changes.optJSONObject(changes.length() - 1);
                if (change != null && !change.isNull("previous")) row(observation, "Last state change · " + transitionTime(dsp, change),
                    encoderLabel(change.optString("previous")) + " → " + encoderLabel(change.optString("state")), null);
            }
            row(observation, "Selected playback", host.codecSummary(), null);
            encoderHistory(screen, dsp, state);
            PreferenceCategory more = category(screen, "More");
            row(more, "Details", "Timing, signal, observation source and freshness", () -> host.show("dsp_details"));
        }
        private void dspDetails(PreferenceScreen screen) {
            JSONObject dsp = host.snapshot.optJSONObject("dsp"); if (dsp == null) dsp = new JSONObject();
            boolean available = host.dspCurrent(dsp);
            tips(screen, available ? "Current observation" : "Waiting for encoder data", available
                ? "Matched to the current playback session" : host.observationHint());
            PreferenceCategory observation = category(screen, "Observation");
            row(observation, "Status", available ? "Live" : dsp.optString("reason", "No current encoder log"), null);
            JSONObject rawState = host.currentDspField(dsp, "encoder_state");
            row(observation, "ENCLEVEL (raw)", rawState == null ? "Not currently observed" : rawState.optString("value"), null);
            if (host.observationError != null) row(observation, "Observation issue", host.observationError, null);
            JSONObject loader = dsp.optJSONObject("loader");
            if (loader != null) row(observation, "DSP loader", loader.optString("message", loader.optString("code", "Unknown")), () -> host.dialog("DSP loader", pretty(host.snapshot.optJSONObject("dsp"))));
            row(observation, "Receiver bit-perfect", "Unverified", null);
            row(observation, "About encoder levels", "ENCLEVEL is the encoder's exact state log. Some HQ and low-latency paths report bitrate feedback instead; it is shown separately.", null);
            row(observation, "About bitrate", "The frame estimate uses one encoder summary's size, channel count and interval. It describes the selected frame budget, before output completes. Output buffer bytes are logged separately and include the common wrapper. Radio traffic, retransmissions and delivered audio are not measured.", null);
            JSONObject fields = dsp.optJSONObject("fields");
            if (fields != null && available) {
                String[] priority = {"encoder_frame_bitrate_bps", "encoder_output_buffer_bytes", "packet_payload_bytes", "channels", "packet_interval_ms", "calculated_ttp_us", "hq_target_word", "latency_state", "system_mode", "sample_rate_hz", "qhs", "rssi_dbm", "feedback_quality_level"};
                List<String> keys = new ArrayList<>();
                for (String key : priority) if (fields.has(key)) keys.add(key);
                for (Iterator<String> all = fields.keys(); all.hasNext();) {
                    String key = all.next();
                    if (!keys.contains(key) && !key.equals("encoder_state") && !key.equals("encoder_level") &&
                        !key.equals("encoder_lossless") && !key.equals("quality_level")) keys.add(key);
                }
                for (String key : keys) {
                    JSONObject field = fields.optJSONObject(key); if (field == null) continue;
                    String value = field.isNull("value") ? "Unknown" : String.valueOf(field.opt("value"));
                    boolean fresh = host.currentDspField(dsp, key) != null;
                    String summary = (key.equals("encoder_frame_bitrate_bps") ? bitrateLabel(field) : value + " " + field.optString("unit", "")) + " · " + (fresh ? "Current" : "Stale");
                    row(observation, dspFieldLabel(key), summary, () -> host.dialog(dspFieldLabel(key), pretty(field)));
                }
            } else row(observation, "Timing and signal", "Appear automatically when the encoder emits these fields", null);
            encoderHistory(screen, dsp, host.currentEncoderState(dsp));
            JSONObject timeline = available ? dsp : dsp.optJSONObject("history");
            JSONArray transitions = timeline == null ? null : timeline.optJSONArray("encoder_transitions");
            if (transitions != null && transitions.length() > 0) {
                PreferenceCategory changes = category(screen, "Observed state history");
                for (int i = transitions.length() - 1; i >= 0; i--) {
                    JSONObject transition = transitions.optJSONObject(i); if (transition == null) continue;
                    String before = transition.isNull("previous") ? "First observed" : transition.optString("previous");
                    row(changes, transitionTime(timeline, transition), before + " → " + transition.optString("state"),
                        () -> host.dialog("Encoder state observation", pretty(transition)));
                }
                row(changes, "Source", "Encoder packet summaries · Observed times only; a state change alone does not confirm an audio interruption.", null);
            }
            PreferenceCategory inspect = category(screen, "Inspect");
            row(inspect, "Copy current status", "Structured JSON with field freshness and sources", () -> host.copy("Shizuka status", pretty(host.snapshot)));
            row(inspect, "Saved diagnostics", "Capture and export detailed logs", () -> host.show("diagnostics"));
        }
        private void encoderHistory(PreferenceScreen screen, JSONObject dsp, String state) {
            JSONObject history = dsp.optJSONObject("history");
            JSONObject historyFields = history == null ? null : history.optJSONObject("fields");
            JSONObject oldState = historyFields == null ? null : historyFields.optJSONObject("encoder_state");
            JSONObject oldQuality = historyFields == null ? null : historyFields.optJSONObject("quality_level");
            if (state == null && ((oldState != null && !oldState.isNull("value")) ||
                    (oldQuality != null && !oldQuality.isNull("value")))) {
                PreferenceCategory saved = category(screen, "Last captured · history");
                row(saved, "ENCLEVEL at capture", oldState == null || oldState.isNull("value") ?
                    "No encoder-state log in this capture" : (host.page.equals("dsp_details") ? oldState.optString("value") : encoderLabel(oldState.optString("value"))) + "\n" + captureTime(history), null);
                if (oldQuality != null && !oldQuality.isNull("value"))
                    row(saved, "Quality at capture", "qlevel " + oldQuality.optString("value") + "\n" + captureTime(history), null);
                row(saved, "Capture context", history.optBoolean("same_session")
                    ? "Same module session · historical, not live"
                    : "Earlier module session · historical, not live", null);
            }
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
            tips(screen, "Module connection", host.error == null && host.updatedAt > 0 ? "Connected to the installed Shizuka module through a bounded root session." : "Allow root access in KernelSU, then reconnect Shizuka.");
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
            row(built, "Settings app version", "0.11.0-test", null);
            row(built, "Observation model", "Saved requests, Android selection and DSP observations are separate states.", null);
        }
    }
}
