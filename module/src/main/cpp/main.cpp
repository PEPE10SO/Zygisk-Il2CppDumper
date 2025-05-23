#include <cstring>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cinttypes>
#include <string> // Para std::string y strlen
#include <cerrno> // Para strerror

#include "hack.h"
#include "zygisk.hpp"
// game.h ya no es necesario para GamePackageName, pero podría tener otras definiciones.
// #include "game.h" 
#include "log.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
public:
    MyModule() : api(nullptr), env(nullptr), enable_hack(false), game_data_dir(nullptr), arm_so_data(nullptr), arm_so_length(0) {
        LOGI("MyModule instance created.");
    }

    ~MyModule() {
        LOGI("MyModule instance destroyed.");
        if (game_data_dir) {
            delete[] game_data_dir;
            game_data_dir = nullptr;
        }
        if (arm_so_data && arm_so_length > 0) {
            LOGD("MyModule destructor: munmap'ing arm_so_data that was not consumed or cleaned up earlier.");
            munmap(arm_so_data, arm_so_length);
            arm_so_data = nullptr;
            arm_so_length = 0;
        }
    }

    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env; // Guardamos el JNIEnv proporcionado por Zygisk
        LOGI("MyModule onLoad completed.");
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        // Reiniciar el estado para la especialización de esta app
        this->enable_hack = false;
        if (this->game_data_dir) {
            delete[] this->game_data_dir;
            this->game_data_dir = nullptr;
        }
        // Si arm_so_data fue mapeado previamente y no limpiado (ej. hack no habilitado en postAppSpecialize)
        if (this->arm_so_data && this->arm_so_length > 0) {
            munmap(this->arm_so_data, this->arm_so_length);
            this->arm_so_data = nullptr;
            this->arm_so_length = 0;
        }

        auto current_package_name_jstr = args->nice_name;
        auto current_app_data_dir_jstr = args->app_data_dir;

        const char *current_package_name_utf = nullptr;
        const char *current_app_data_dir_utf = nullptr;

        if (current_package_name_jstr) {
            current_package_name_utf = env->GetStringUTFChars(current_package_name_jstr, nullptr);
        }
        // app_data_dir también se necesita si el hack se habilita
        if (current_app_data_dir_jstr && current_package_name_utf) { // Solo obtener si el package name también está disponible
            current_app_data_dir_utf = env->GetStringUTFChars(current_app_data_dir_jstr, nullptr);
        }


        if (current_package_name_utf) {
            LOGI("Zygisk Il2Cpp Dumper evaluating package: %s", current_package_name_utf);

            if (hasIl2CppLibrary(current_package_name_utf)) { // Pasamos el nombre del paquete para logging
                LOGI("libil2cpp.so check positive for %s. Enabling dumper.", current_package_name_utf);
                this->enable_hack = true;

                if (current_app_data_dir_utf) {
                    size_t app_data_dir_len = strlen(current_app_data_dir_utf);
                    this->game_data_dir = new char[app_data_dir_len + 1];
                    strcpy(this->game_data_dir, current_app_data_dir_utf);
                    this->game_data_dir[app_data_dir_len] = '\0';
                    LOGI("Game data directory set to: %s", this->game_data_dir);
                } else {
                    LOGE("App data directory is null for %s, but libil2cpp.so was detected. Disabling hack.", current_package_name_utf);
                    this->enable_hack = false; // Necesitamos app_data_dir
                }

                // Solo proceder con la carga del helper ARM si el hack está habilitado
                if (this->enable_hack) {
#if defined(__i386__) || defined(__x86_64__)
                    const char* path = nullptr;
#if defined(__i386__)
                    path = "zygisk/armeabi-v7a.so";
                    LOGI("Targeting armeabi-v7a helper for x86.");
#elif defined(__x86_64__)
                    path = "zygisk/arm64-v8a.so";
                    LOGI("Targeting arm64-v8a helper for x86_64.");
#endif
                    int dirfd = api->getModuleDir();
                    if (dirfd != -1) {
                        int fd = openat(dirfd, path, O_RDONLY);
                        if (fd != -1) {
                            struct stat sb{};
                            if (fstat(fd, &sb) == 0) {
                                arm_so_length = sb.st_size;
                                if (arm_so_length > 0) {
                                    arm_so_data = mmap(nullptr, arm_so_length, PROT_READ, MAP_PRIVATE, fd, 0);
                                    if (arm_so_data == MAP_FAILED) {
                                        LOGE("mmap failed for %s: %s. Disabling hack.", path, strerror(errno));
                                        arm_so_data = nullptr;
                                        arm_so_length = 0;
                                        this->enable_hack = false; // Error crítico
                                    } else {
                                        LOGI("Successfully mmap'd helper library %s, size %zu", path, arm_so_length);
                                    }
                                } else { LOGW("Helper library %s is empty.", path); arm_so_data = nullptr; arm_so_length = 0; }
                            } else { LOGE("fstat failed for %s: %s", path, strerror(errno)); arm_so_data = nullptr; arm_so_length = 0; }
                            close(fd);
                        } else { LOGW("Unable to open ARM helper library %s: %s", path, strerror(errno)); arm_so_data = nullptr; arm_so_length = 0; }
                    } else { LOGE("Failed to get module directory descriptor."); arm_so_data = nullptr; arm_so_length = 0; }
#else
                    LOGI("Running on ARM architecture. No helper .so mmap needed in main module.");
                    arm_so_data = nullptr;
                    arm_so_length = 0;
#endif
                }
            } else {
                LOGI("libil2cpp.so NOT detected for %s. Dumper will remain inactive.", current_package_name_utf);
                this->enable_hack = false; // Asegurar que está deshabilitado
                // Asegurar que los recursos del helper ARM no queden mapeados si no se usarán
                this->arm_so_data = nullptr;
                this->arm_so_length = 0;
            }
        } else {
            LOGE("Failed to get package name. Dumper cannot be evaluated for this app.");
            this->enable_hack = false;
        }

        if (current_package_name_utf) {
            env->ReleaseStringUTFChars(current_package_name_jstr, current_package_name_utf);
        }
        if (current_app_data_dir_utf) {
            env->ReleaseStringUTFChars(current_app_data_dir_jstr, current_app_data_dir_utf);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (enable_hack && game_data_dir) {
            LOGI("Hack is enabled for %s. Preparing hack in new thread.", game_data_dir);
            std::thread hack_thread(hack_prepare, game_data_dir, arm_so_data, arm_so_length);
            hack_thread.detach();
            // La responsabilidad de arm_so_data (munmap) se transfiere a hack_prepare/NativeBridgeLoad.
            // Anulamos los punteros aquí para indicar que la propiedad se ha transferido.
            arm_so_data = nullptr;
            arm_so_length = 0;
        } else {
            LOGI("Hack not enabled or game_data_dir not set for this app (%s). Dumper inactive.", this->game_data_dir ? this->game_data_dir : "unknown data dir");
            // Si arm_so_data fue mapeado pero el hack no se va a ejecutar (o falló en preApp), liberarlo.
            if (arm_so_data && arm_so_length > 0) {
                LOGD("postAppSpecialize: munmap'ing arm_so_data because hack is not proceeding.");
                munmap(arm_so_data, arm_so_length);
                arm_so_data = nullptr;
                arm_so_length = 0;
            }
        }
    }

private:
    Api *api;
    JNIEnv *env; // Este JNIEnv es establecido en onLoad
    bool enable_hack;
    char *game_data_dir;
    void *arm_so_data;
    size_t arm_so_length;

    // Nueva función helper para verificar la existencia de libil2cpp.so
    bool hasIl2CppLibrary(const char* current_package_name_for_log) {
        if (!this->env) { // Usar el JNIEnv almacenado en la instancia
            LOGE("JNIEnv is null in MyModule::hasIl2CppLibrary for %s", current_package_name_for_log);
            return false;
        }
        JNIEnv* current_jni_env = this->env;

        std::string native_lib_dir_str;

        // --- Lógica JNI para obtener nativeLibraryDir ---
        // Esta es una adaptación del código que tenías en GetLibDir y de la discusión
        jclass activity_thread_clz = current_jni_env->FindClass("android/app/ActivityThread");
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during FindClass(ActivityThread) for %s", current_package_name_for_log); }
        if (!activity_thread_clz) {
            LOGE("hasIl2CppLibrary: ActivityThread class not found for %s", current_package_name_for_log);
            return false;
        }

        jmethodID currentApplicationId = current_jni_env->GetStaticMethodID(activity_thread_clz, "currentApplication", "()Landroid/app/Application;");
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetStaticMethodID(currentApplication) for %s", current_package_name_for_log); }
        current_jni_env->DeleteLocalRef(activity_thread_clz); // Limpiar ref de clase
        if (!currentApplicationId) {
            LOGE("hasIl2CppLibrary: currentApplication method ID not found for %s", current_package_name_for_log);
            return false;
        }

        jobject application_obj = current_jni_env->CallStaticObjectMethod(activity_thread_clz, currentApplicationId);
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during CallStaticObjectMethod(currentApplication) for %s", current_package_name_for_log); }
        if (!application_obj) {
            LOGE("hasIl2CppLibrary: currentApplication() returned null for %s", current_package_name_for_log);
            return false;
        }

        jclass application_clazz = current_jni_env->GetObjectClass(application_obj);
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetObjectClass(application_obj) for %s", current_package_name_for_log); }
        if (!application_clazz) {
            LOGE("hasIl2CppLibrary: Failed to get Application class for %s", current_package_name_for_log);
            current_jni_env->DeleteLocalRef(application_obj);
            return false;
        }

        jmethodID get_application_info_id = current_jni_env->GetMethodID(application_clazz, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetMethodID(getApplicationInfo) for %s", current_package_name_for_log); }
        current_jni_env->DeleteLocalRef(application_clazz); // Limpiar ref de clase
        if (!get_application_info_id) {
            LOGE("hasIl2CppLibrary: getApplicationInfo method ID not found for %s", current_package_name_for_log);
            current_jni_env->DeleteLocalRef(application_obj);
            return false;
        }

        jobject application_info_obj = current_jni_env->CallObjectMethod(application_obj, get_application_info_id);
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during CallObjectMethod(getApplicationInfo) for %s", current_package_name_for_log); }
        current_jni_env->DeleteLocalRef(application_obj); // Limpiar ref de objeto
        if (!application_info_obj) {
            LOGE("hasIl2CppLibrary: getApplicationInfo() returned null for %s", current_package_name_for_log);
            return false;
        }

        jclass application_info_clazz = current_jni_env->GetObjectClass(application_info_obj);
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetObjectClass(application_info_obj) for %s", current_package_name_for_log); }
        if (!application_info_clazz) {
            LOGE("hasIl2CppLibrary: Failed to get ApplicationInfo class for %s", current_package_name_for_log);
            current_jni_env->DeleteLocalRef(application_info_obj);
            return false;
        }

        jfieldID native_library_dir_id = current_jni_env->GetFieldID(application_info_clazz, "nativeLibraryDir", "Ljava/lang/String;");
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetFieldID(nativeLibraryDir) for %s", current_package_name_for_log); }
        current_jni_env->DeleteLocalRef(application_info_clazz); // Limpiar ref de clase
        if (!native_library_dir_id) {
            LOGE("hasIl2CppLibrary: nativeLibraryDir field ID not found for %s", current_package_name_for_log);
            current_jni_env->DeleteLocalRef(application_info_obj);
            return false;
        }

        jstring native_library_dir_jstring = (jstring) current_jni_env->GetObjectField(application_info_obj, native_library_dir_id);
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); LOGE("JNI Exception during GetObjectField(nativeLibraryDir) for %s", current_package_name_for_log); }
        current_jni_env->DeleteLocalRef(application_info_obj); // Limpiar ref de objeto

        if (native_library_dir_jstring) {
            const char* path_utf = current_jni_env->GetStringUTFChars(native_library_dir_jstring, nullptr);
            if (path_utf) {
                native_lib_dir_str = path_utf;
                current_jni_env->ReleaseStringUTFChars(native_library_dir_jstring, path_utf);
            }
            current_jni_env->DeleteLocalRef(native_library_dir_jstring);
        } else {
            LOGI("hasIl2CppLibrary: nativeLibraryDir is a null jstring for %s.", current_package_name_for_log);
        }
        // Limpiar cualquier excepción pendiente de la secuencia JNI.
        if (current_jni_env->ExceptionCheck()) { current_jni_env->ExceptionClear(); }


        if (native_lib_dir_str.empty()) {
            LOGI("hasIl2CppLibrary: Native library directory path is empty for %s. Assuming libil2cpp.so is not present via this path.", current_package_name_for_log);
            return false;
        }
        // --- Fin lógica JNI ---

        std::string lib_to_check = "libil2cpp.so";
        std::string full_lib_path = native_lib_dir_str + "/" + lib_to_check;

        LOGI("hasIl2CppLibrary: For %s, checking for library at: %s", current_package_name_for_log, full_lib_path.c_str());
        struct stat buffer;
        if (stat(full_lib_path.c_str(), &buffer) == 0) {
            // Verificar que sea un archivo regular y no un directorio (aunque dlopen se encargaría de eso)
            if (S_ISREG(buffer.st_mode)) {
                 LOGI("hasIl2CppLibrary: Found %s via stat at %s.", lib_to_check.c_str(), full_lib_path.c_str());
                 return true;
            } else {
                 LOGW("hasIl2CppLibrary: Path %s exists but is not a regular file. Assuming %s not present correctly.", full_lib_path.c_str(), lib_to_check.c_str());
                 return false;
            }
        } else {
            LOGI("hasIl2CppLibrary: %s NOT found via stat for %s at %s (stat error: %s). Dumper will remain inactive.",
                 lib_to_check.c_str(), current_package_name_for_log, full_lib_path.c_str(), strerror(errno));
            return false;
        }
    }
};

REGISTER_ZYGISK_MODULE(MyModule)
