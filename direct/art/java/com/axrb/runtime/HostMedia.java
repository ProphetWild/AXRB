package com.axrb.runtime;

import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.view.Surface;
import java.lang.ref.WeakReference;
import java.util.concurrent.ConcurrentHashMap;

/** Routes the AOSP MediaPlayer native boundary to the Windows media backend. */
public final class HostMedia {
    // Keep the backend state until release/finalize. A WeakHashMap can discard
    // the entry before MediaPlayer's finalizer has closed the native decoder.
    private static final ConcurrentHashMap<Long, State> players = new ConcurrentHashMap<>();
    private static final class State {
        volatile long id;
        volatile int session;
        WeakReference<MediaPlayer> owner;
        volatile boolean started, released;
    }
    static native long command(int operation, long id, long a, long b, long c);
    static native long readFrame(long surface, long destination, int width, int height, long previous);
    static native void releaseSurface(long surface);
    // The host frame slot for a Surface made from a SurfaceTexture, or 0
    // (direct/art/art_surface_texture.c shows the frames on its GL texture).
    static native long attachTexture(Surface surface, long player);
    private static State state(MediaPlayer player) {
        try {
            State state = players.get((Long)Reflect.field(player, "mNativeContext"));
            if (state == null || state.released) throw new IllegalStateException("MediaPlayer released");
            return state;
        } catch (Exception error) { throw new IllegalStateException(error); }
    }
    public static void setup(MediaPlayer player, Object reference, int session) throws Exception {
        State state = new State();
        state.id = command(0, 0, 0, 0, 0);
        state.session = session > 0 ? session : (int)state.id;
        state.owner = new WeakReference<>(player);
        players.put(state.id, state);
        Reflect.set(player, "mNativeContext", state.id);
    }
    private static void event(State state, int what, int a, int b) {
        if (state.released) return;
        try { Reflect.call(MediaPlayer.class, "postEventFromNative", state.owner, what, a, b, null); }
        catch (Exception error) { throw new IllegalStateException(error); }
    }
    public static long control(MediaPlayer player, int op, long a, long b, long c) throws Exception {
        if (op == 12 && (Long)Reflect.field(player, "mNativeContext") == 0L) return 0;
        State state = state(player);
        if (op == 100) return state.session;
        if (op == 101) { state.session = (int)a; return 0; }
        if (op == 12) {
            state.released = true;
            players.remove(state.id);
            Reflect.set(player, "mNativeContext", 0L);
        }
        if (op == 103) {
            new Thread(() -> {
                long result = command(3, state.id, 0, 0, 0);
                if (result == 0) {
                    event(state, 5, (int)command(13, state.id, 0, 0, 0), (int)command(14, state.id, 0, 0, 0));
                    event(state, 1, 0, 0);
                } else event(state, 100, 1, -1004);
            }, "AXRB media prepare").start();
            return 0;
        }
        if (op == 104) {
            command(12, state.id, 0, 0, 0);
            players.remove(state.id);
            state.id = command(0, 0, 0, 0, 0);
            players.put(state.id, state);
            state.started = false;
            Reflect.set(player, "mNativeContext", state.id);
            return 0;
        }
        long result = command(op, state.id, a, b, c);
        if (result < 0) throw new java.io.IOException("Media operation " + op + " failed");
        if (op == 3) event(state, 5, (int)command(13, state.id, 0, 0, 0), (int)command(14, state.id, 0, 0, 0));
        if (op == 7) event(state, 4, 0, 0);
        if (op == 5 || op == 6) state.started = false;
        if (op == 4 && !state.started) {
            state.started = true;
            Handler handler = new Handler(Looper.getMainLooper());
            handler.postDelayed(new Runnable() {
                public void run() {
                    if (!state.started || state.released || state.owner.get() == null) return;
                    if (command(9, state.id, 0, 0, 0) == 0) { state.started = false; event(state, 2, 0, 0); }
                    else handler.postDelayed(this, 30);
                }
            }, 30);
        }
        return result;
    }
    public static void surface(MediaPlayer player, Surface surface) {
        // AXRB host surfaces and Surfaces made from a SurfaceTexture (a GL
        // external texture, as Unreal's movie player uses) show the host
        // decoder's frames. Any other plays the audio and completes without
        // picture: throwing here would surface as a Java exception inside the
        // engine's JNI call, which Unreal aborts on.
        State state = state(player);
        long handle = 0;
        if (surface instanceof HostSurface) handle = ((HostSurface)surface).mediaHandle;
        else if (surface != null) {
            handle = attachTexture(surface, state.id);
            if (handle == 0) android.util.Log.w("AXRB.Media", "MediaPlayer output is not an AXRB host surface; video plays without picture");
        }
        command(2, state.id, handle, 0, 0);
    }
    public static int invoke(MediaPlayer player, Parcel request, Parcel reply) {
        State state = state(player);
        request.setDataPosition(0);
        request.enforceInterface("android.media.IMediaPlayer");
        int operation = request.readInt();
        // The host decoder's tracks as Android lists them: one video track once
        // prepared, no audio, subtitle or timed-text tracks. Unreal's movie
        // player finds video only through getTrackInfo(); with an empty list it
        // never selects the track, never fetches a frame and its media time
        // never advances (Star Wars: Tales from the Galaxy's Edge waits on it).
        boolean video = command(13, state.id, 0, 0, 0) > 0;
        if (reply != null) {
            if (operation == 1) { // INVOKE_ID_GET_TRACK_INFO, read as Parcel.createTypedArray
                reply.writeInt(video ? 1 : 0);
                if (video) {
                    reply.writeInt(1); // non-null element
                    reply.writeInt(1); // MEDIA_TRACK_TYPE_VIDEO
                    reply.writeString("video/avc");
                    reply.writeString("und");
                }
            } else if (operation == 7) { // INVOKE_ID_GET_SELECTED_TRACK: the video track is always on
                reply.writeInt(request.readInt() == 1 && video ? 0 : -1);
            } else {
                reply.writeInt(0);
            }
            reply.setDataPosition(0);
        }
        // 4 and 5 select or deselect a track: accepted, the single video track stays on.
        return operation == 1 || operation == 4 || operation == 5 || operation == 6 || operation == 7 || operation == 8 ? 0 : -38;
    }
}
