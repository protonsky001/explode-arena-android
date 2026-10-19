/* JNI glue between com.explodearena.port.Native and the engine host (ea_host.c). */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "ea_host.h"

#define FN(name) Java_com_explodearena_port_Native_##name

JNIEXPORT jint JNICALL FN(init)(JNIEnv *env, jclass cls, jbyteArray code, jbyteArray head, jint pakTotal) {
    (void)cls;
    jsize cl = (*env)->GetArrayLength(env, code), hl = (*env)->GetArrayLength(env, head);
    jbyte *cp = (*env)->GetByteArrayElements(env, code, NULL);
    jbyte *hp = (*env)->GetByteArrayElements(env, head, NULL);
    int r = ea_init((const uint8_t *)cp, (size_t)cl, (const uint8_t *)hp, (size_t)hl, (size_t)pakTotal);
    (*env)->ReleaseByteArrayElements(env, code, cp, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, head, hp, JNI_ABORT);
    ea_set_strict(0);
    return r;
}

JNIEXPORT void JNICALL FN(setAsset)(JNIEnv *env, jclass cls, jint idx, jbyteArray data) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *p = (*env)->GetByteArrayElements(env, data, NULL);
    ea_set_asset_copy(idx, (const uint8_t *)p, (uint32_t)n);
    (*env)->ReleaseByteArrayElements(env, data, p, JNI_ABORT);
}

JNIEXPORT void JNICALL FN(reset)(JNIEnv *env, jclass cls) { (void)env; (void)cls; ea_reset(); }

JNIEXPORT jint JNICALL FN(boot)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return ea_boot(); }

JNIEXPORT jint JNICALL FN(frame)(JNIEnv *env, jclass cls, jint ticks) {
    (void)env; (void)cls;
    ea_set_ticks((uint32_t)ticks);
    return ea_frame();
}

JNIEXPORT jint JNICALL FN(key)(JNIEnv *env, jclass cls, jint scan, jboolean down) { (void)env; (void)cls; return ea_key(scan, down ? 1 : 0); }

/* converts the 176x208 RGB565 view of the engine bitmap to ARGB_8888 */
JNIEXPORT jint JNICALL FN(copyScreen)(JNIEnv *env, jclass cls, jintArray dst) {
    (void)cls;
    static uint16_t px[176 * 208];
    static jint out[176 * 208];
    if (ea_copy_screen(px) != 0) return -1;
    for (int i = 0; i < 176 * 208; i++) {
        uint32_t p = px[i], r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        out[i] = (jint)(0xff000000u | (r << 16) | (g << 8) | b);
    }
    (*env)->SetIntArrayRegion(env, dst, 0, 176 * 208, out);
    return 0;
}

JNIEXPORT jint JNICALL FN(audioRead)(JNIEnv *env, jclass cls, jshortArray dst, jint n) {
    (void)cls;
    static int16_t tmp[8192];
    if (n > 8192) n = 8192;
    int rate = ea_audio_read(tmp, n);
    (*env)->SetShortArrayRegion(env, dst, 0, n, (const jshort *)tmp);
    return rate;
}

JNIEXPORT jint JNICALL FN(audioRate)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return ea_audio_rate(); }
JNIEXPORT jboolean JNICALL FN(quitRequested)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return ea_quit_requested() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jint JNICALL FN(saveConfig)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return ea_save_config(); }
JNIEXPORT jboolean JNICALL FN(configDirty)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return ea_config_dirty() ? JNI_TRUE : JNI_FALSE; }

JNIEXPORT jbyteArray JNICALL FN(getConfig)(JNIEnv *env, jclass cls) {
    (void)cls;
    uint32_t n = 0; const uint8_t *c = ea_config(&n);
    jbyteArray a = (*env)->NewByteArray(env, (jsize)n);
    if (a && n) (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, (const jbyte *)c);
    return a;
}

JNIEXPORT void JNICALL FN(setConfig)(JNIEnv *env, jclass cls, jbyteArray data) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *p = (*env)->GetByteArrayElements(env, data, NULL);
    ea_set_config((const uint8_t *)p, (uint32_t)n);
    (*env)->ReleaseByteArrayElements(env, data, p, JNI_ABORT);
}

JNIEXPORT jstring JNICALL FN(lastError)(JNIEnv *env, jclass cls) { (void)cls; return (*env)->NewStringUTF(env, ea_last_error()); }
