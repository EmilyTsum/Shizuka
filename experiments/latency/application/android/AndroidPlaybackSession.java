// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.content.*;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.*;
import android.os.Parcel;
import android.os.Parcelable;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;

/** Real Android service adapter. Never constructed by the unqualified CLI factory. */
final class AndroidPlaybackSession implements AutoCloseable {
    interface Profiles { BluetoothProfile a2dp() throws Exception; }
    interface Authority { void requireCurrent() throws Exception; }
    static final class Captured {
        BluetoothDevice peer;
        CodecSnapshot codec;
        int selectedRate;
        boolean playing,pausedByUs,targetEligible;
        MediaController owner;
        MediaSession.Token token;
        String mediaIdentity;
        long queueId,pauseUpdate;
    }
    private final BluetoothRuntime runtime;
    private final Profiles profiles;
    private final Authority authority;
    private final AudioManager audio;
    private final MediaSessionManager sessions;
    private volatile long codecEvents;
    private final BroadcastReceiver receiver;
    private boolean closed;
    AndroidPlaybackSession(BluetoothRuntime runtime,Profiles profiles,Authority authority)throws Exception {
        this.runtime=runtime;this.profiles=profiles;this.authority=authority;
        audio=runtime.context.getSystemService(AudioManager.class);
        sessions=runtime.context.getSystemService(MediaSessionManager.class);
        if(audio==null||sessions==null)throw new IllegalStateException("Audio or media session service unavailable");
        receiver=new BroadcastReceiver(){public void onReceive(Context c,Intent i){synchronized(AndroidPlaybackSession.this){codecEvents++;}}};
        // This is a protected framework action. Any unexpected event only makes
        // restoration more conservative; an event can never authorize a write.
        runtime.context.registerReceiver(receiver,new IntentFilter("android.bluetooth.a2dp.profile.action.CODEC_CONFIG_CHANGED"),Context.RECEIVER_EXPORTED);
    }
    private BluetoothProfile a2dp()throws Exception{return profiles.a2dp();}
    private List<BluetoothDevice> active()throws Exception {
        Object value=BluetoothRuntime.call(runtime.adapter,"getActiveDevices",new Class<?>[]{int.class},BluetoothProfile.A2DP);
        if(!(value instanceof Iterable))throw new IllegalStateException("Active A2DP inventory unavailable");
        List<BluetoothDevice> out=new ArrayList<>();for(Object item:(Iterable<?>)value) {
            if(!(item instanceof BluetoothDevice))throw new IllegalStateException("Invalid active A2DP inventory");
            if(!out.contains(item))out.add((BluetoothDevice)item);
        }return out;
    }
    private Object status(BluetoothDevice peer)throws Exception {
        Object result=BluetoothRuntime.call(a2dp(),"getCodecStatus",new Class<?>[]{BluetoothDevice.class},peer);
        if(result==null)throw new IllegalStateException("Actual codec status unavailable");return result;
    }
    private Object selected(BluetoothDevice peer)throws Exception {
        return BluetoothRuntime.call(status(peer),"getCodecConfig",new Class<?>[]{});
    }
    private List<MediaController> media() {
        // MEDIA_CONTENT_CONTROL is checked by Android here. Failure is a
        // preflight failure, never a reason to use a global media key.
        List<MediaController> list=sessions.getActiveSessions(null);
        if(list==null)throw new IllegalStateException("Media owner inventory unavailable");return list;
    }
    private static boolean local(MediaController controller) {
        MediaController.PlaybackInfo info=controller.getPlaybackInfo();
        return info!=null&&info.getPlaybackType()==MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL;
    }
    private static String identity(MediaController controller)throws Exception {
        MediaMetadata md=controller.getMetadata();
        if(md==null)throw new IllegalStateException("Media identity unavailable");
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        boolean identified=false;
        for(String key:new String[]{MediaMetadata.METADATA_KEY_MEDIA_ID,MediaMetadata.METADATA_KEY_TITLE,
                MediaMetadata.METADATA_KEY_ARTIST,MediaMetadata.METADATA_KEY_ALBUM}) {
            String value=md.getString(key);if(value==null)value="";
            if(!value.isEmpty())identified=true;
            byte[] bytes=value.getBytes(StandardCharsets.UTF_8);
            digest.update(new byte[]{(byte)(bytes.length>>>24),(byte)(bytes.length>>>16),(byte)(bytes.length>>>8),(byte)bytes.length});digest.update(bytes);
        }
        if(!identified)throw new IllegalStateException("Media identity is empty");
        digest.update(Long.toString(md.getLong(MediaMetadata.METADATA_KEY_DURATION)).getBytes(StandardCharsets.US_ASCII));
        return hex(digest.digest());
    }
    private static String hex(byte[] bytes){StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format(Locale.ROOT,"%02x",b&255));return out.toString();}
    private static int rate(Object codec)throws Exception {
        int flag=(Integer)BluetoothRuntime.call(codec,"getSampleRate",new Class<?>[]{});
        for(int hz:new int[]{44100,48000,88200,96000,176400,192000}) {
            try {if(codec.getClass().getField("SAMPLE_RATE_"+hz).getInt(null)==flag)return hz;}
            catch(NoSuchFieldException unsupported) { /* A selected unknown rate remains unsupported. */ }
        }return 0;
    }
    private static boolean mediaRouteA2dp()throws Exception {
        Class<?> system=Class.forName("android.media.AudioSystem");
        int devices=((Number)system.getMethod("getDevicesForStream",int.class).invoke(null,AudioManager.STREAM_MUSIC)).intValue(),mask=0;
        for(java.lang.reflect.Field field:system.getFields())if(java.lang.reflect.Modifier.isStatic(field.getModifiers())&&field.getType()==int.class&&field.getName().startsWith("DEVICE_OUT_BLUETOOTH_A2DP"))mask|=field.getInt(null);
        return mask!=0&&(devices&mask)!=0&&(devices&~mask)==0;
    }
    Captured capture(boolean allowNoPeer)throws Exception {
        authority.requireCurrent();
        if(closed||!runtime.adapter.isEnabled()||audio.getMode()!=AudioManager.MODE_NORMAL)
            throw new IllegalStateException("Bluetooth must be enabled in normal audio mode");
        List<BluetoothDevice> connected=a2dp().getConnectedDevices(),active=active();
        Captured captured=new Captured();
        if(connected.isEmpty()&&active.isEmpty()&&allowNoPeer&&!audio.isMusicActive())return captured;
        if(connected.size()!=1||active.size()!=1||!connected.get(0).equals(active.get(0)))
            throw new IllegalStateException("No unique connected active A2DP peer");
        captured.peer=active.get(0);
        if(captured.peer.getBondState()!=BluetoothDevice.BOND_BONDED)throw new IllegalStateException("Active peer is not bonded");
        Object codec=selected(captured.peer);captured.codec=CodecSnapshot.capture(codec);captured.codec.cloneObject();captured.selectedRate=rate(codec);
        captured.targetEligible=CodecTargetEligibility.inspect(codec,BluetoothRuntime.call(status(captured.peer),"getCodecsSelectableCapabilities",new Class<?>[]{})).eligible;
        List<MediaController> playing=new ArrayList<>();
        for(MediaController controller:media()) {
            PlaybackState state=controller.getPlaybackState();
            if(state!=null&&state.getState()==PlaybackState.STATE_PLAYING&&local(controller))playing.add(controller);
        }
        captured.playing=audio.isMusicActive();
        if(captured.playing&&!mediaRouteA2dp())throw new IllegalStateException("Active music is not routed exclusively to A2DP");
        if(captured.playing!=(!playing.isEmpty()) || playing.size()>1)throw new IllegalStateException("Playback owner is ambiguous");
        if(captured.playing) {
            captured.owner=playing.get(0);captured.token=captured.owner.getSessionToken();
            PlaybackState state=captured.owner.getPlaybackState();
            if(state==null||state.getState()!=PlaybackState.STATE_PLAYING ||
                    (state.getActions()&PlaybackState.ACTION_PAUSE)==0 || (state.getActions()&PlaybackState.ACTION_PLAY)==0)
                throw new IllegalStateException("Exact media owner cannot pause and resume");
            captured.mediaIdentity=identity(captured.owner);captured.queueId=state.getActiveQueueItemId();
        }
        if(check(captured,false)!=ApplyOperation.Fence.SAME)throw new IllegalStateException("Session changed during capture");
        return captured;
    }
    private boolean mediaUnchanged(Captured c)throws Exception {
        if(audio.getMode()!=AudioManager.MODE_NORMAL)return false;
        if(!c.playing)return !audio.isMusicActive();
        boolean found=false;
        for(MediaController controller:media()) {
            if(c.token.equals(controller.getSessionToken()))found=true;
            PlaybackState current=controller.getPlaybackState();
            if(local(controller)&&current!=null&&current.getState()==PlaybackState.STATE_PLAYING&&!c.token.equals(controller.getSessionToken()))return false;
        }
        PlaybackState state=c.owner.getPlaybackState();
        if(!found||!c.token.equals(c.owner.getSessionToken())||state==null||!c.mediaIdentity.equals(identity(c.owner))||state.getActiveQueueItemId()!=c.queueId)return false;
        return c.pausedByUs?state.getState()==PlaybackState.STATE_PAUSED&&state.getLastPositionUpdateTime()==c.pauseUpdate:
            state.getState()==PlaybackState.STATE_PLAYING;
    }
    ApplyOperation.Fence check(Captured c,boolean allowOwnedDisconnect)throws Exception {
        authority.requireCurrent();
        if(closed||!mediaUnchanged(c))return ApplyOperation.Fence.USER_CHANGED;
        if(c.playing&&!allowOwnedDisconnect&&!mediaRouteA2dp())return ApplyOperation.Fence.USER_CHANGED;
        List<BluetoothDevice> current=active();
        if(c.peer==null)return current.isEmpty()&&!audio.isMusicActive()?ApplyOperation.Fence.SAME:ApplyOperation.Fence.USER_CHANGED;
        if(c.peer.getBondState()!=BluetoothDevice.BOND_BONDED)return ApplyOperation.Fence.USER_CHANGED;
        for(BluetoothDevice device:current)if(!device.equals(c.peer))return ApplyOperation.Fence.USER_CHANGED;
        List<BluetoothDevice> connected=a2dp().getConnectedDevices();
        for(BluetoothDevice device:connected)if(!device.equals(c.peer))return ApplyOperation.Fence.USER_CHANGED;
        if(!runtime.adapter.isEnabled()||!connected.contains(c.peer)||current.size()!=1)
            return allowOwnedDisconnect?ApplyOperation.Fence.OWN_TRANSIENT:ApplyOperation.Fence.UNKNOWN;
        // The qualified plan preserves the Bluetooth generation. A different
        // connected selection is not an authorized HAL transient.
        return c.codec.matches(selected(c.peer))?ApplyOperation.Fence.SAME:ApplyOperation.Fence.USER_CHANGED;
    }
    void pauseOwned(Captured c,long deadline)throws Exception {
        if(!c.playing)return;
        if(check(c,false)!=ApplyOperation.Fence.SAME)throw new IllegalStateException("Media intent changed before quiesce");
        c.owner.getTransportControls().pause();
        long end=Math.min(deadline,now()+3000);
        while(now()<end) {
            authority.requireCurrent();
            PlaybackState state=c.owner.getPlaybackState();
            if(state!=null&&state.getState()==PlaybackState.STATE_PAUSED&&c.mediaIdentity.equals(identity(c.owner))&&state.getActiveQueueItemId()==c.queueId) {
                c.pauseUpdate=state.getLastPositionUpdateTime();c.pausedByUs=true;
                if(mediaUnchanged(c))return;break;
            }
            Thread.sleep(50);
        }throw new IllegalStateException("Exact media owner did not confirm owned pause");
    }
    String codecFingerprint(Captured c)throws Exception {
        Object codec=selected(c.peer);
        if(!(codec instanceof Parcelable))throw new IllegalStateException("Codec parcel identity unavailable");
        Parcel parcel=Parcel.obtain();
        try {((Parcelable)codec).writeToParcel(parcel,0);return hex(MessageDigest.getInstance("SHA-256").digest(parcel.marshall()));}
        finally {parcel.recycle();}
    }
    long codecEventSequence(){return codecEvents;}
    void restore(Captured c,boolean resume,String expectedOwnedReset,long permittedCodecEvents,long deadline)throws Exception {
        if(c.peer!=null)reconnect(c,deadline);
        if(c.peer!=null&&!c.codec.matches(selected(c.peer))) {
            if(expectedOwnedReset==null||!expectedOwnedReset.matches("[a-f0-9]{64}")||!expectedOwnedReset.equals(codecFingerprint(c))||permittedCodecEvents!=codecEvents)
                throw new IllegalStateException("Changed codec has no matching operation-owned reset permit");
            Object reset=selected(c.peer);CodecSnapshot resetSnapshot=CodecSnapshot.capture(reset);long sequence=codecEvents;
            Object capabilities=BluetoothRuntime.call(status(c.peer),"getCodecsSelectableCapabilities",new Class<?>[]{});
            List<Object> list=new ArrayList<>();
            if(capabilities instanceof Iterable)for(Object item:(Iterable<?>)capabilities)list.add(item);
            else if(capabilities!=null&&capabilities.getClass().isArray())for(int i=0;i<java.lang.reflect.Array.getLength(capabilities);i++)list.add(java.lang.reflect.Array.get(capabilities,i));
            else throw new IllegalStateException("Selectable codec inventory unavailable");
            boolean advertised=false;for(Object candidate:list)advertised|=c.codec.advertisedBy(candidate);
            if(!advertised||!mediaUnchanged(c)||!resetSnapshot.matches(selected(c.peer))||sequence!=codecEvents)
                throw new IllegalStateException("Codec capabilities or manual selection changed before restoration");
            Object clone=c.codec.cloneObject();
            authority.requireCurrent();
            Object accepted=BluetoothRuntime.call(a2dp(),"setCodecConfigPreference",new Class<?>[]{BluetoothDevice.class,clone.getClass()},c.peer,clone);
            if(Boolean.FALSE.equals(accepted))throw new IllegalStateException("Android rejected exact codec restoration");
            long end=Math.min(deadline,now()+5000);
            while(now()<end&&!c.codec.matches(selected(c.peer))) {
                authority.requireCurrent();
                if(!mediaUnchanged(c))throw new IllegalStateException("Media intent changed during codec restoration");Thread.sleep(100);
            }
            if(!c.codec.matches(selected(c.peer)))throw new IllegalStateException("Exact codec restoration was not observed");
        }
        if(check(c,false)!=ApplyOperation.Fence.SAME)throw new IllegalStateException("Session changed before media restoration");
        if(resume) {
            if(!c.playing||!c.pausedByUs||!mediaUnchanged(c)||!mediaRouteA2dp())throw new IllegalStateException("No unchanged owned A2DP pause to resume");
            c.owner.getTransportControls().play();
            long end=Math.min(deadline,now()+3000);
            while(now()<end) {
                authority.requireCurrent();
                PlaybackState state=c.owner.getPlaybackState();
                if(state!=null&&state.getState()==PlaybackState.STATE_PLAYING&&c.mediaIdentity.equals(identity(c.owner))&&state.getActiveQueueItemId()==c.queueId) {
                    c.pausedByUs=false;if(mediaUnchanged(c))return;break;
                }Thread.sleep(50);
            }throw new IllegalStateException("Exact media owner did not confirm resumed playback");
        }
    }
    private void reconnect(Captured c,long deadline)throws Exception {
        boolean connect=false,activate=false;long end=Math.min(deadline,now()+20000);
        while(now()<end) {
            authority.requireCurrent();
            if(!mediaUnchanged(c)||c.peer.getBondState()!=BluetoothDevice.BOND_BONDED)throw new IllegalStateException("Peer or media intent changed");
            List<BluetoothDevice> active=active();for(BluetoothDevice other:active)if(!other.equals(c.peer))throw new IllegalStateException("Another peer became active");
            List<BluetoothDevice> connected=a2dp().getConnectedDevices();for(BluetoothDevice other:connected)if(!other.equals(c.peer))throw new IllegalStateException("Another peer connected");
            if(runtime.adapter.isEnabled()) {
                if(connected.contains(c.peer)) {
                    if(active.size()==1)return;
                    if(!activate&&active.isEmpty()) {
                        activate=true;Object okay=BluetoothRuntime.call(a2dp(),"setActiveDevice",new Class<?>[]{BluetoothDevice.class},c.peer);
                        if(Boolean.FALSE.equals(okay))throw new IllegalStateException("Android rejected captured-peer activation");
                    }
                } else if(!connect&&a2dp().getConnectionState(c.peer)==BluetoothProfile.STATE_DISCONNECTED) {
                    connect=true;Object okay=BluetoothRuntime.call(a2dp(),"connect",new Class<?>[]{BluetoothDevice.class},c.peer);
                    if(Boolean.FALSE.equals(okay)&&a2dp().getConnectionState(c.peer)!=BluetoothProfile.STATE_CONNECTING)
                        throw new IllegalStateException("Android rejected captured bonded peer connection");
                }
            }Thread.sleep(100);
        }throw new IllegalStateException("Captured peer did not reconnect uniquely before deadline");
    }
    private static long now(){return System.nanoTime()/1000000L;}
    public void close(){if(!closed){closed=true;runtime.context.unregisterReceiver(receiver);}}
}
