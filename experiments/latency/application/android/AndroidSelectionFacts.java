// SPDX-License-Identifier: GPL-2.0-only
import android.bluetooth.*;
import android.os.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Fixed read-only full actual peer/codec fingerprint; no preference/connect/media writes. */
public final class AndroidSelectionFacts {
    private static boolean done;
    private static void finish(BluetoothRuntime runtime,BluetoothProfile profile,Exception error) {
        if(done)return;done=true;int exit=0;
        try {
            if(error!=null)throw error;
            List<BluetoothDevice> connected=profile.getConnectedDevices();
            Object active=BluetoothRuntime.call(runtime.adapter,"getActiveDevices",new Class<?>[]{int.class},BluetoothProfile.A2DP);
            if(!(active instanceof List))throw new IllegalStateException("Actual active peer list unavailable");
            List<?> list=(List<?>)active;
            if(!runtime.adapter.isEnabled()||connected.size()!=1||list.size()!=1||!connected.get(0).equals(list.get(0))||connected.get(0).getBondState()!=BluetoothDevice.BOND_BONDED) {
                System.out.println(ControlJson.encode(ControlJson.object("schema",1,"scope","application_selection","unique",false)));return;
            }
            BluetoothDevice peer=connected.get(0);
            Object status=BluetoothRuntime.call(profile,"getCodecStatus",new Class<?>[]{BluetoothDevice.class},peer);
            Object codec=BluetoothRuntime.call(status,"getCodecConfig",new Class<?>[]{});
            CodecSnapshot exact=CodecSnapshot.capture(codec);exact.cloneObject();
            CodecTargetEligibility target=CodecTargetEligibility.inspect(codec,BluetoothRuntime.call(status,"getCodecsSelectableCapabilities",new Class<?>[]{}));
            if(!(codec instanceof Parcelable)||!codec.getClass().getName().equals("android.bluetooth.BluetoothCodecConfig"))throw new IllegalStateException("Unexpected framework codec class");
            Parcel parcel=Parcel.obtain();byte[] bytes;
            try{((Parcelable)codec).writeToParcel(parcel,0);bytes=parcel.marshall();}finally{parcel.recycle();}
            if(bytes.length<1||bytes.length>4096)throw new IllegalStateException("Codec parcel exceeds bound");
            int rate=0,flag=(Integer)BluetoothRuntime.call(codec,"getSampleRate",new Class<?>[]{});
            for(int hz:new int[]{44100,48000,88200,96000,176400,192000})try{if(codec.getClass().getField("SAMPLE_RATE_"+hz).getInt(null)==flag)rate=hz;}catch(NoSuchFieldException ignored){}
            // Recheck the actual object and peer immediately before output.
            if(!exact.matches(BluetoothRuntime.call(BluetoothRuntime.call(profile,"getCodecStatus",new Class<?>[]{BluetoothDevice.class},peer),"getCodecConfig",new Class<?>[]{}))||
                !profile.getConnectedDevices().equals(connected)||!BluetoothRuntime.call(runtime.adapter,"getActiveDevices",new Class<?>[]{int.class},BluetoothProfile.A2DP).equals(list))throw new IllegalStateException("Selection changed during read");
            System.out.println(ControlJson.encode(ControlJson.object("schema",1,"scope","application_selection","unique",true,
                "peer_sha256",ApplicationInputs.sha(peer.getAddress().toUpperCase(Locale.ROOT).getBytes(StandardCharsets.US_ASCII)),
                "codec_sha256",ApplicationInputs.sha(bytes),"selected_rate_hz",rate,"codec_type",target.type,"codec_specific1",target.mode,
                "selection_bits",target.bits,"capability_compatible",target.capabilityCompatible,"target_eligible",target.eligible)));
        }catch(Exception failure){System.err.println("Actual selection unavailable: "+failure.getClass().getSimpleName());exit=1;}
        finally{if(profile!=null)runtime.adapter.closeProfileProxy(BluetoothProfile.A2DP,profile);System.exit(exit);}
    }
    public static void main(String[] args) {
        try {
            if(args.length!=0)throw new IllegalArgumentException("This fixed reader takes no arguments");
            BluetoothRuntime runtime=BluetoothRuntime.connect();Handler handler=new Handler(Looper.getMainLooper());
            BluetoothProfile.ServiceListener listener=new BluetoothProfile.ServiceListener(){public void onServiceConnected(int id,BluetoothProfile proxy){handler.post(()->finish(runtime,proxy,null));}public void onServiceDisconnected(int id){}};
            handler.postDelayed(()->finish(runtime,null,new IllegalStateException("A2DP read timeout")),3000);
            if(!runtime.adapter.getProfileProxy(runtime.context,listener,BluetoothProfile.A2DP))throw new IllegalStateException("A2DP proxy request rejected");
            Looper.loop();
        }catch(Exception unavailable){System.err.println("Selection reader unavailable: "+unavailable.getClass().getSimpleName());System.exit(1);}
    }
}
