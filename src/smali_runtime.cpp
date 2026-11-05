#include "smali_runtime.h"
#include "logger.h"
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <vector>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

extern JavaVM* g_jvm_global;

static bool   s_ready    = false;
static jclass s_main_cls = nullptr;

static std::string extract_jar(JNIEnv* env) {
    jclass   act = env->FindClass("android/app/ActivityThread");
    jobject  app = env->CallStaticObjectMethod(act,
        env->GetStaticMethodID(act, "currentApplication",
                               "()Landroid/app/Application;"));
    jobject  am  = env->CallObjectMethod(app,
        env->GetMethodID(env->FindClass("android/content/Context"),
                         "getAssets",
                         "()Landroid/content/res/AssetManager;"));
    jstring  fds = (jstring)env->CallObjectMethod(app,
        env->GetMethodID(env->FindClass("android/content/Context"),
                         "getFilesDir", "()Ljava/io/File;"));
    fds = (jstring)env->CallObjectMethod(fds,
        env->GetMethodID(env->FindClass("java/io/File"),
                         "getAbsolutePath", "()Ljava/lang/String;"));
    const char* fdp = env->GetStringUTFChars(fds, nullptr);
    char jar[512];
    snprintf(jar, sizeof(jar), "%s/smali-fat.jar", fdp);
    env->ReleaseStringUTFChars(fds, fdp);
    if (access(jar, F_OK) == 0) return jar;
    AAssetManager* amgr = AAssetManager_fromJava(env, am);
    AAsset* a = AAssetManager_open(amgr, "smali-fat.jar", AASSET_MODE_BUFFER);
    if (!a) return "";
    FILE* f = fopen(jar, "wb");
    if (!f) { AAsset_close(a); return ""; }
    fwrite(AAsset_getBuffer(a), 1, (size_t)AAsset_getLength(a), f);
    fclose(f);
    AAsset_close(a);
    return std::string(jar);
}

static bool ensure_ready(JNIEnv* env) {
    if (s_ready) return true;
    std::string jar = extract_jar(env);
    if (jar.empty()) return false;
    jclass   cl  = env->FindClass("java/lang/ClassLoader");
    jobject  sc  = env->CallStaticObjectMethod(cl,
        env->GetStaticMethodID(cl, "getSystemClassLoader",
                               "()Ljava/lang/ClassLoader;"));
    jclass   dcl = env->FindClass("dalvik/system/PathClassLoader");
    jstring  js  = env->NewStringUTF(jar.c_str());
    jobject  lo  = env->NewObject(dcl,
        env->GetMethodID(dcl, "<init>",
            "(Ljava/lang/String;Ljava/lang/ClassLoader;)V"), js, sc);
    if (!lo) return false;
    jstring  cn  = env->NewStringUTF("com.android.tools.smali.smali.Main");
    s_main_cls   = (jclass)env->CallObjectMethod(lo,
        env->GetMethodID(cl, "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;"), cn);
    if (!s_main_cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    s_main_cls = (jclass)env->NewGlobalRef(s_main_cls);
    s_ready = true;
    return true;
}

std::string smali_compile(const std::string& src,
                          const std::string& cname,
                          std::vector<uint8_t>& out) {
    JNIEnv* env = nullptr;
    if (!g_jvm_global) return "no JVM";
    g_jvm_global->AttachCurrentThread(&env, nullptr);
    if (!ensure_ready(env)) return "smali not ready";
    char tmp[] = "/data/local/tmp/dxt-XXXXXX";
    if (!mkdtemp(tmp)) return "mkdtemp failed";
    std::string sf = std::string(tmp) + "/" + cname + ".smali";
    std::string df = std::string(tmp) + "/out.dex";
    FILE* f = fopen(sf.c_str(), "w");
    if (!f) return "fopen failed";
    fwrite(src.c_str(), 1, src.size(), f);
    fclose(f);
    jclass       sc = env->FindClass("java/lang/String");
    jmethodID    m  = env->GetStaticMethodID(s_main_cls, "main",
                                              "([Ljava/lang/String;)V");
    if (!m) return "no main method";
    jobjectArray a  = env->NewObjectArray(4, sc, nullptr);
    env->SetObjectArrayElement(a, 0, env->NewStringUTF("assemble"));
    env->SetObjectArrayElement(a, 1, env->NewStringUTF("-o"));
    env->SetObjectArrayElement(a, 2, env->NewStringUTF(df.c_str()));
    env->SetObjectArrayElement(a, 3, env->NewStringUTF(sf.c_str()));
    env->CallStaticVoidMethod(s_main_cls, m, a);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return "smali threw"; }
    FILE* df2 = fopen(df.c_str(), "rb");
    if (!df2) return "no dex output";
    fseek(df2, 0, SEEK_END);
    long sz = ftell(df2);
    rewind(df2);
    out.resize((size_t)sz);
    fread(out.data(), 1, (size_t)sz, df2);
    fclose(df2);
    return "";
}
