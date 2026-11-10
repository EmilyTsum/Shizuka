#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real Android service adapter against small synthetic API objects."""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from shizuka.java import java_tools

STUBS = {
    "android/content/Context.java": '''package android.content;
public class Context {public static final int RECEIVER_EXPORTED=2;public java.util.Map<Class<?>,Object> services=new java.util.HashMap<>();public BroadcastReceiver receiver;public int closed;
 public <T>T getSystemService(Class<T> c){return c.cast(services.get(c));}public Intent registerReceiver(BroadcastReceiver r,IntentFilter f,int flags){receiver=r;return null;}public void unregisterReceiver(BroadcastReceiver r){closed++;}}
''',
    "android/content/BroadcastReceiver.java": "package android.content;public abstract class BroadcastReceiver{public abstract void onReceive(Context c,Intent i);}",
    "android/content/Intent.java": "package android.content;public class Intent{}",
    "android/content/IntentFilter.java": "package android.content;public class IntentFilter{public IntentFilter(String s){}}",
    "android/bluetooth/BluetoothProfile.java": '''package android.bluetooth;public interface BluetoothProfile{int A2DP=2,STATE_DISCONNECTED=0,STATE_CONNECTING=1;java.util.List<BluetoothDevice> getConnectedDevices();int getConnectionState(BluetoothDevice d);}''',
    "android/bluetooth/BluetoothDevice.java": '''package android.bluetooth;public class BluetoothDevice{public static final int BOND_BONDED=12;public int bond=12;final String address;public BluetoothDevice(String a){address=a;}public int getBondState(){return bond;}public String getAddress(){return address;}}''',
    "android/bluetooth/BluetoothAdapter.java": '''package android.bluetooth;public class BluetoothAdapter{public boolean enabled=true;public java.util.List<BluetoothDevice> active=new java.util.ArrayList<>();public boolean isEnabled(){return enabled;}public java.util.List<BluetoothDevice> getActiveDevices(int id){return active;}}''',
    "android/media/AudioManager.java": "package android.media;public class AudioManager{public static final int MODE_NORMAL=0,STREAM_MUSIC=3;public int mode;public boolean music;public int getMode(){return mode;}public boolean isMusicActive(){return music;}}",
    "android/media/AudioSystem.java": "package android.media;public class AudioSystem{public static final int DEVICE_OUT_BLUETOOTH_A2DP=128;public static int devices=128;public static int getDevicesForStream(int stream){return devices;}}",
    "android/media/MediaMetadata.java": '''package android.media;public class MediaMetadata {public static final String METADATA_KEY_MEDIA_ID="id",METADATA_KEY_TITLE="title",METADATA_KEY_ARTIST="artist",METADATA_KEY_ALBUM="album",METADATA_KEY_DURATION="duration";public String title="Track";public String getString(String key){return key.equals("title")?title:"";}public long getLong(String key){return 180000;}}''',
    "android/media/session/MediaSession.java": "package android.media.session;public class MediaSession{public static class Token{}}",
    "android/media/session/PlaybackState.java": '''package android.media.session;public class PlaybackState{public static final int STATE_PLAYING=3,STATE_PAUSED=2;public static final long ACTION_PLAY=4,ACTION_PAUSE=2;public int state=3;public long update=1,actions=6,queue=5;public int getState(){return state;}public long getActions(){return actions;}public long getActiveQueueItemId(){return queue;}public long getLastPositionUpdateTime(){return update;}}''',
    "android/media/session/MediaController.java": '''package android.media.session;
public class MediaController{public final MediaSession.Token token=new MediaSession.Token();public final PlaybackState state=new PlaybackState();public final android.media.MediaMetadata metadata=new android.media.MediaMetadata();public final android.media.AudioManager audio;public int pauses,plays;public MediaController(android.media.AudioManager a){audio=a;}public MediaSession.Token getSessionToken(){return token;}public PlaybackState getPlaybackState(){return state;}public android.media.MediaMetadata getMetadata(){return metadata;}public String getPackageName(){return "example.music";}public PlaybackInfo getPlaybackInfo(){return new PlaybackInfo();}public TransportControls getTransportControls(){return new TransportControls();}
 public static class PlaybackInfo{public static final int PLAYBACK_TYPE_LOCAL=1;public int getPlaybackType(){return 1;}}
 public class TransportControls{public void pause(){pauses++;state.state=2;state.update++;audio.music=false;}public void play(){plays++;state.state=3;state.update++;audio.music=true;}}}
''',
    "android/media/session/MediaSessionManager.java": '''package android.media.session;public class MediaSessionManager{public java.util.List<MediaController> list=new java.util.ArrayList<>();public boolean denied;public java.util.List<MediaController> getActiveSessions(Object unused){if(denied)throw new SecurityException("denied");return list;}}''',
    "android/os/Parcelable.java": "package android.os;public interface Parcelable{void writeToParcel(Parcel p,int flags);}",
    "android/os/Parcel.java": '''package android.os;public class Parcel{final java.io.ByteArrayOutputStream b=new java.io.ByteArrayOutputStream();final java.io.DataOutputStream d=new java.io.DataOutputStream(b);public static Parcel obtain(){return new Parcel();}public void writeInt(int v){try{d.writeInt(v);}catch(Exception e){throw new RuntimeException(e);}}public void writeLong(long v){try{d.writeLong(v);}catch(Exception e){throw new RuntimeException(e);}}public byte[] marshall(){return b.toByteArray();}public void recycle(){}}''',
    "android/bluetooth/BluetoothCodecConfig.java": '''package android.bluetooth;
public class BluetoothCodecConfig implements android.os.Parcelable{public static final int SAMPLE_RATE_44100=1,SAMPLE_RATE_96000=8,BITS_PER_SAMPLE_16=1,BITS_PER_SAMPLE_24=2;public int type=1,priority=123,rate=1,bits=2,channels=2;public long a=1197,b=-2,c=Long.MIN_VALUE,d=Long.MAX_VALUE;
 public int getCodecType(){return type;}public int getCodecPriority(){return priority;}public int getSampleRate(){return rate;}public int getBitsPerSample(){return bits;}public int getChannelMode(){return channels;}public long getCodecSpecific1(){return a;}public long getCodecSpecific2(){return b;}public long getCodecSpecific3(){return c;}public long getCodecSpecific4(){return d;}
 public void writeToParcel(android.os.Parcel p,int f){p.writeInt(type);p.writeInt(priority);p.writeInt(rate);p.writeInt(bits);p.writeInt(channels);p.writeLong(a);p.writeLong(b);p.writeLong(c);p.writeLong(d);}
 public static class Builder{final BluetoothCodecConfig c=new BluetoothCodecConfig();public Builder setCodecType(int v){c.type=v;return this;}public Builder setCodecPriority(int v){c.priority=v;return this;}public Builder setSampleRate(int v){c.rate=v;return this;}public Builder setBitsPerSample(int v){c.bits=v;return this;}public Builder setChannelMode(int v){c.channels=v;return this;}public Builder setCodecSpecific1(long v){c.a=v;return this;}public Builder setCodecSpecific2(long v){c.b=v;return this;}public Builder setCodecSpecific3(long v){c.c=v;return this;}public Builder setCodecSpecific4(long v){c.d=v;return this;}public BluetoothCodecConfig build(){return c;}}}
''',
    "BluetoothRuntime.java": '''final class BluetoothRuntime {final android.content.Context context;final android.bluetooth.BluetoothAdapter adapter;BluetoothRuntime(android.content.Context c,android.bluetooth.BluetoothAdapter a){context=c;adapter=a;}static Object call(Object o,String n,Class<?>[] t,Object...a)throws Exception{return o.getClass().getMethod(n,t).invoke(o,a);}}''',
}

TEST = r'''
import android.bluetooth.*;import android.media.*;import android.media.session.*;
public class AndroidSessionTest{
 static int checks;interface Checked{void run()throws Exception;}static void check(boolean v){checks++;if(!v)throw new AssertionError("Android check "+checks);}static void rejects(Checked action)throws Exception{try{action.run();throw new AssertionError("unsafe acceptance");}catch(IllegalStateException|SecurityException expected){checks++;}}
 public static class Status {final Profile p;Status(Profile p){this.p=p;}public Object getCodecConfig(){return p.codec;}public Object[] getCodecsSelectableCapabilities(){return new Object[]{p.original};}}
 public static class Profile implements BluetoothProfile {java.util.List<BluetoothDevice> connected=new java.util.ArrayList<>();BluetoothCodecConfig codec=new BluetoothCodecConfig(),original=codec;BluetoothAdapter adapter;int connects,activates,writes;Profile(BluetoothAdapter a){adapter=a;}public java.util.List<BluetoothDevice> getConnectedDevices(){return connected;}public int getConnectionState(BluetoothDevice d){return connected.contains(d)?2:0;}public Status getCodecStatus(BluetoothDevice d){return new Status(this);}public boolean connect(BluetoothDevice d){connects++;connected.add(d);return true;}public boolean setActiveDevice(BluetoothDevice d){activates++;adapter.active.add(d);return true;}public boolean setCodecConfigPreference(BluetoothDevice d,BluetoothCodecConfig c){writes++;codec=c;return true;}}
 static class Fixture implements AutoCloseable {android.content.Context context=new android.content.Context();BluetoothAdapter bt=new BluetoothAdapter();AudioManager audio=new AudioManager();MediaSessionManager media=new MediaSessionManager();MediaController controller=new MediaController(audio);BluetoothDevice peer=new BluetoothDevice("00:11:22:33:44:55");Profile profile=new Profile(bt);boolean authority=true;AndroidPlaybackSession service;Fixture(boolean playing)throws Exception{audio.music=playing;controller.state.state=playing?3:2;media.list.add(controller);context.services.put(AudioManager.class,audio);context.services.put(MediaSessionManager.class,media);bt.active.add(peer);profile.connected.add(peer);service=new AndroidPlaybackSession(new BluetoothRuntime(context,bt),()->profile,()->{if(!authority)throw new IllegalStateException("guardian lost");});}public void close(){service.close();}}
 static long deadline(){return System.nanoTime()/1000000+2000;}
 public static void main(String[] args)throws Exception{
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);check(c.playing&&c.selectedRate==44100&&c.peer==f.peer);f.service.pauseOwned(c,deadline());check(f.controller.pauses==1&&c.pausedByUs);f.service.restore(c,true,null,-1,deadline());check(f.controller.plays==1&&f.profile.writes==0&&f.service.check(c,false)==ApplyOperation.Fence.SAME);}
  try(Fixture f=new Fixture(false)){f.profile.codec.rate=8;AndroidPlaybackSession.Captured c=f.service.capture(false);check(!c.playing&&c.selectedRate==96000);f.service.pauseOwned(c,deadline());f.service.restore(c,false,null,-1,deadline());check(f.controller.pauses==0&&f.controller.plays==0&&f.profile.codec.rate==8);}
  try(Fixture f=new Fixture(false)){f.profile.codec.type=7;f.profile.codec.a=4096;AndroidPlaybackSession.Captured c=f.service.capture(false);check(c.targetEligible&&!c.playing&&c.selectedRate==44100);}
  try(Fixture f=new Fixture(true)){f.media.list.add(new MediaController(f.audio));rejects(()->f.service.capture(false));check(f.controller.pauses==0);}
  try(Fixture f=new Fixture(true)){AudioSystem.devices=2;rejects(()->f.service.capture(false));check(f.controller.pauses==0);AudioSystem.devices=128;}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());AudioSystem.devices=130;rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.controller.plays==0);AudioSystem.devices=128;}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.profile.codec=new BluetoothCodecConfig();f.profile.codec.type=7;check(f.service.check(c,true)==ApplyOperation.Fence.USER_CHANGED);}
  try(Fixture f=new Fixture(true)){f.media.list.clear();rejects(()->f.service.capture(false));}
  try(Fixture f=new Fixture(true)){f.controller.state.actions=2;rejects(()->f.service.capture(false));}
  try(Fixture f=new Fixture(true)){f.media.denied=true;rejects(()->f.service.capture(false));}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.controller.metadata.title="Changed";rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.controller.plays==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.controller.state.update++;rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.controller.plays==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.bt.active.clear();f.bt.active.add(new BluetoothDevice("00:11:22:33:44:66"));rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.controller.plays==0&&f.profile.connects==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.peer.bond=10;rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.profile.connects==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.profile.codec=new BluetoothCodecConfig();f.profile.codec.type=0;String reset=f.service.codecFingerprint(c);f.service.restore(c,true,reset,f.service.codecEventSequence(),deadline());check(f.profile.writes==1&&f.controller.plays==1);check(c.codec.matches(f.profile.codec)&&f.profile.codec.priority==123&&f.profile.codec.b==-2&&f.profile.codec.c==Long.MIN_VALUE&&f.profile.codec.d==Long.MAX_VALUE);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.profile.codec=new BluetoothCodecConfig();f.profile.codec.type=0;rejects(()->f.service.restore(c,true,null,-1,deadline()));check(f.profile.writes==0&&f.controller.plays==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.profile.codec=new BluetoothCodecConfig();f.profile.codec.type=0;String reset=f.service.codecFingerprint(c);long events=f.service.codecEventSequence();f.context.receiver.onReceive(f.context,new android.content.Intent());rejects(()->f.service.restore(c,true,reset,events,deadline()));check(f.profile.writes==0);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.service.pauseOwned(c,deadline());f.bt.active.clear();f.profile.connected.clear();f.service.restore(c,true,null,-1,deadline());check(f.profile.connects==1&&f.profile.activates==1&&f.controller.plays==1);}
  try(Fixture f=new Fixture(true)){AndroidPlaybackSession.Captured c=f.service.capture(false);f.authority=false;rejects(()->f.service.pauseOwned(c,deadline()));check(f.controller.pauses==0);}
  try(Fixture f=new Fixture(false)){f.bt.active.clear();f.profile.connected.clear();f.media.denied=true;AndroidPlaybackSession.Captured c=f.service.capture(true);check(c.peer==null&&!c.playing);f.service.restore(c,false,null,-1,deadline());check(f.profile.connects==0&&f.controller.plays==0);f.service.close();check(f.context.closed==1);}
  System.out.println("Actual Android adapter source: "+checks+" synthetic service/codec/media-intent checks passed.");
 }
}
'''

java, compiler = java_tools()
with tempfile.TemporaryDirectory(prefix="shizuka-android-adapter-") as temporary:
    out = Path(temporary)
    sources = []
    for name, text in STUBS.items():
        path = out / "sources" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        sources.append(path)
    test = out / "AndroidSessionTest.java"
    test.write_text(TEST)
    subprocess.run([*compiler, "--release", "8", "-d", str(out / "classes"),
                    *map(str, sources), str(test), str(HERE / "CodecSnapshot.java"),
                    str(HERE / "ApplyOperation.java"), str(HERE / "CodecTargetEligibility.java"), str(HERE / "android/AndroidPlaybackSession.java")], check=True)
    subprocess.run([java, "-cp", str(out / "classes"), "AndroidSessionTest"], check=True, timeout=15)
