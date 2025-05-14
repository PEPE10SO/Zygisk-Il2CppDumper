// Copyright (c) 2020-2021 Perfare
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Created by Perfare on 2020/7/4.

#include "hack.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h> // Para __system_property_get y android_get_device_api_level
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h> // Para __NR_memfd_create
#include <sys/syscall.h>  // Para syscall
#include <array>
#include <string>         // Para std::string
#include <linux/limits.h> // Para PATH_MAX
#include <cerrno>         // Para strerror

// Definición de android_get_device_api_level si no está disponible globalmente
// (generalmente está en <android/api-level.h>, que xdl.c ya incluye)
// extern "C" int android_get_device_api_level();


void hack_start(const char *game_data_dir) {
    LOGI("hack_start invoked for game_data_dir: %s", game_data_dir ? game_data_dir : "null");
    if (!game_data_dir) {
        LOGE("game_data_dir is null in hack_start. Aborting.");
        return;
    }

    bool load = false;
    for (int i = 0; i < 10; i++) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            LOGI("libil2cpp.so loaded successfully at try %d. Handle: %p", i + 1, handle);
            load = true;
            il2cpp_api_init(handle); // il2cpp_api_init ya contiene la lógica de inicialización y obtención de la base.
            il2cpp_dump(game_data_dir);
            // xdl_close(handle); // Considerar si cerrar el handle aquí o dejarlo para el sistema.
            // Si il2cpp_dump usa funciones de la librería después de la inicialización, no cerrar.
            break;
        } else {
            LOGW("Failed to load libil2cpp.so at try %d. Waiting 1 second.", i + 1);
            sleep(1);
        }
    }
    if (!load) {
        LOGE("libil2cpp.so not found after 10 tries in thread %d.", gettid());
    }
}

std::string GetLibDir(JavaVM *vms) {
    if (!vms) {
        LOGE("JavaVM is null in GetLibDir");
        return {};
    }
    JNIEnv *env = nullptr;
    if (vms->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("Failed to attach current thread to JavaVM or env is null.");
        return {};
    }

    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            if (application) {
                jclass application_clazz = env->GetObjectClass(application);
                if (application_clazz) {
                    jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                      "getApplicationInfo",
                                                                      "()Landroid/content/pm/ApplicationInfo;");
                    if (get_application_info) {
                        jobject application_info = env->CallObjectMethod(application,
                                                                         get_application_info);
                        if (application_info) {
                            jfieldID native_library_dir_id = env->GetFieldID(
                                    env->GetObjectClass(application_info), "nativeLibraryDir",
                                    "Ljava/lang/String;");
                            if (native_library_dir_id) {
                                auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                        application_info, native_library_dir_id);
                                if (native_library_dir_jstring) {
                                    const char* path_utf = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                                    if (path_utf) {
                                        LOGI("lib dir from JNI: %s", path_utf);
                                        std::string lib_dir(path_utf);
                                        env->ReleaseStringUTFChars(native_library_dir_jstring, path_utf);
                                        env->DeleteLocalRef(native_library_dir_jstring); // Clean up local ref
                                        // Clean up other local refs
                                        env->DeleteLocalRef(application_info);
                                        env->DeleteLocalRef(application_clazz);
                                        env->DeleteLocalRef(application);
                                        env->DeleteLocalRef(activity_thread_clz);
                                        // DetachCurrentThread no es necesario aquí si la función es llamada desde un hilo nativo
                                        // que se adjuntó temporalmente. Si es un hilo persistente, se debe considerar.
                                        // vms->DetachCurrentThread(); // Opcional, dependiendo del ciclo de vida del hilo.
                                        return lib_dir;
                                    }
                                    env->DeleteLocalRef(native_library_dir_jstring);
                                }
                            } else {
                                LOGE("nativeLibraryDir field ID not found");
                            }
                            env->DeleteLocalRef(application_info);
                        }
                    } else {
                        LOGE("getApplicationInfo method ID not found");
                    }
                    env->DeleteLocalRef(application_clazz);
                }
                env->DeleteLocalRef(application);
            }
        } else {
            LOGE("currentApplication method ID not found");
        }
        env->DeleteLocalRef(activity_thread_clz);
    } else {
        LOGE("ActivityThread class not found");
    }
    // vms->DetachCurrentThread(); // Si se adjuntó
    return {};
}


static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;
    void *(*loadLibrary)(const char *libpath, int flag);
    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);
    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;
    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *arm_so_data_param, size_t arm_so_length_param) {
    LOGI("NativeBridgeLoad called. game_data_dir: %s, api_level: %d", game_data_dir ? game_data_dir : "null", api_level);

    if (!arm_so_data_param || arm_so_length_param == 0) {
        LOGW("NativeBridgeLoad: No ARM helper library data provided or data is empty.");
        // No se puede proceder a cargar la librería ARM si no hay datos.
        // No es necesario llamar a munmap aquí porque los datos no son válidos para empezar, o no fueron mapeados.
        return false;
    }

    // Esperar a que Houdini (o el puente nativo) se inicialice.
    LOGI("Waiting 5 seconds for native bridge initialization...");
    sleep(5);

    auto libart_handle = dlopen("libart.so", RTLD_NOW);
    if (!libart_handle) {
        LOGE("Failed to dlopen libart.so: %s", dlerror());
        munmap(arm_so_data_param, arm_so_length_param); // Liberar los datos del .so si no podemos continuar.
        return false;
    }

    auto JNI_GetCreatedJavaVMs_ptr = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(libart_handle, "JNI_GetCreatedJavaVMs");
    dlclose(libart_handle); // Cerrar el handle de libart.so después de obtener el símbolo.

    if (!JNI_GetCreatedJavaVMs_ptr) {
        LOGE("JNI_GetCreatedJavaVMs symbol not found in libart.so.");
        munmap(arm_so_data_param, arm_so_length_param);
        return false;
    }
    LOGI("JNI_GetCreatedJavaVMs_ptr: %p", JNI_GetCreatedJavaVMs_ptr);

    JavaVM *vms_buf[1];
    JavaVM *vms = nullptr;
    jsize num_vms;
    jint status = JNI_GetCreatedJavaVMs_ptr(vms_buf, 1, &num_vms);
    if (status == JNI_OK && num_vms > 0) {
        vms = vms_buf[0];
        LOGI("Successfully got JavaVM: %p", vms);
    } else {
        LOGE("Failed to get JavaVMs. Status: %d, num_vms: %d", status, num_vms);
        munmap(arm_so_data_param, arm_so_length_param);
        return false;
    }

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) {
        LOGE("GetLibDir failed or returned empty path.");
        munmap(arm_so_data_param, arm_so_length_param);
        return false;
    }
    // Si la librería nativa ya es x86 (o la arquitectura del host), no necesitamos el puente.
    if (lib_dir.find("/lib/x86") != std::string::npos || lib_dir.find("/lib/x86_64") != std::string::npos) {
        LOGI("Running on x86/x86_64 architecture, no need for NativeBridge for this library: %s", lib_dir.c_str());
        munmap(arm_so_data_param, arm_so_length_param); // Liberar los datos del .so ARM, ya que no se usarán.
        return false; // Indica que el hack_start principal (x86) debe continuar.
    }

    LOGI("Attempting to load native bridge (e.g., libhoudini.so)...");
    void *nb_handle = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb_handle) {
        auto native_bridge_lib_name = GetNativeBridgeLibrary();
        if (native_bridge_lib_name.empty() || strcmp(native_bridge_lib_name.c_str(),"0") == 0) {
             LOGW("ro.dalvik.vm.native.bridge is not set or is '0'. No native bridge library specified.");
        } else {
            LOGI("Trying to load native bridge specified by system property: %s", native_bridge_lib_name.c_str());
            nb_handle = dlopen(native_bridge_lib_name.c_str(), RTLD_NOW);
        }
    }

    if (nb_handle) {
        LOGI("Native bridge library loaded successfully. Handle: %p", nb_handle);
        auto callbacks = (NativeBridgeCallbacks *) dlsym(nb_handle, "NativeBridgeItf");
        if (callbacks) {
            LOGI("NativeBridgeItf found. Version: %u", callbacks->version);
            LOGI("NativeBridgeLoadLibrary: %p", callbacks->loadLibrary);
            LOGI("NativeBridgeLoadLibraryExt: %p", callbacks->loadLibraryExt);
            LOGI("NativeBridgeGetTrampoline: %p", callbacks->getTrampoline);

            // Crear un memfd para pasar la librería ARM.
            int fd = syscall(__NR_memfd_create, "anon_arm_so", MFD_CLOEXEC);
            if (fd == -1) {
                LOGE("syscall __NR_memfd_create failed: %s", strerror(errno));
                dlclose(nb_handle);
                munmap(arm_so_data_param, arm_so_length_param);
                return false;
            }

            if (ftruncate(fd, (off_t) arm_so_length_param) == -1) {
                 LOGE("ftruncate failed for memfd: %s", strerror(errno));
                 close(fd);
                 dlclose(nb_handle);
                 munmap(arm_so_data_param, arm_so_length_param);
                 return false;
            }

            void *mem = mmap(nullptr, arm_so_length_param, PROT_WRITE, MAP_SHARED, fd, 0);
            if (mem == MAP_FAILED) {
                LOGE("mmap failed for memfd: %s", strerror(errno));
                close(fd);
                dlclose(nb_handle);
                munmap(arm_so_data_param, arm_so_length_param);
                return false;
            }

            memcpy(mem, arm_so_data_param, arm_so_length_param);
            munmap(mem, arm_so_length_param); // Desenmapear la escritura, el kernel mantiene los datos.
            munmap(arm_so_data_param, arm_so_length_param); // Liberar el buffer original.

            char path[PATH_MAX];
            snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);
            LOGI("ARM helper .so prepared at memfd path: %s", path);

            void *arm_helper_handle = nullptr;
            if (api_level >= 26 && callbacks->loadLibraryExt) { // Android O (API 26) y superior
                LOGI("Using NativeBridgeLoadLibraryExt (namespace 3 - CLASSPATH_SHARED_NAMESPACE).");
                // El namespace 3 (APP_NAMESPACE_SHARED) es una suposición común para bibliotecas compartidas por classloader.
                // Documentación oficial sobre los namespaces de NativeBridge es escasa.
                // android_namespace_t* ns = (android_namespace_t*)3; // Esto es conceptual, el valor exacto puede variar o ser específico del puente.
                // El código original usa (void*)3, lo cual podría ser un índice o un tipo de namespace.
                // Si esto falla, podría ser necesario investigar qué valor de namespace es apropiado.
                // Para mayor seguridad, probar primero con nullptr si es posible.
                // Sin embargo, el código original usa `(void *) 3`.
                arm_helper_handle = callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3);
                 if (!arm_helper_handle && callbacks->loadLibrary) { // Fallback si loadLibraryExt falla o no es el adecuado
                    LOGW("NativeBridgeLoadLibraryExt failed or returned null. Trying loadLibrary.");
                    arm_helper_handle = callbacks->loadLibrary(path, RTLD_NOW);
                }
            } else if (callbacks->loadLibrary) {
                LOGI("Using NativeBridgeLoadLibrary.");
                arm_helper_handle = callbacks->loadLibrary(path, RTLD_NOW);
            } else {
                 LOGE("No suitable NativeBridge load function found in callbacks.");
            }


            if (arm_helper_handle) {
                LOGI("ARM helper .so loaded successfully via NativeBridge. Handle: %p", arm_helper_handle);
                // Obtener el JNI_OnLoad de la librería ARM.
                auto init_arm_lib = (jint (*)(JavaVM *, void *)) callbacks->getTrampoline(arm_helper_handle,
                                                                                           "JNI_OnLoad",
                                                                                           nullptr, 0);
                if (init_arm_lib) {
                    LOGI("JNI_OnLoad trampoline obtained from ARM helper .so: %p", init_arm_lib);
                    // Llamar a JNI_OnLoad de la librería ARM.
                    // El segundo argumento `reserved` se usa para pasar `game_data_dir`.
                    jint onload_result = init_arm_lib(vms, (void *) game_data_dir);
                    LOGI("JNI_OnLoad for ARM helper .so returned: %d", onload_result);
                    if (onload_result == JNI_ERR) {
                        LOGE("JNI_OnLoad in ARM helper .so indicated an error.");
                        // Considerar si se debe cerrar el handle de la librería ARM o no.
                        // callbacks->unloadLibrary(arm_helper_handle); // Si está disponible y es seguro.
                    }
                    // No cerrar fd aquí, dlopen mantiene una referencia. Se cerrará cuando el proceso termine o la librería sea descargada.
                    // dlclose(nb_handle); // No cierres el handle del puente aquí si otras partes aún lo necesitan.
                    // La librería ARM ahora está cargada y su JNI_OnLoad ha sido llamado.
                    // Esto significa que el hack_start dentro de la librería ARM se habrá iniciado.
                    // close(fd) se puede hacer aquí si dlopen lo maneja bien.
                    // Generalmente, el fd se puede cerrar después de dlopen si el kernel lo permite para /proc/self/fd.
                    close(fd);
                    return true; // El hack ARM está en camino.
                } else {
                    LOGE("Failed to get JNI_OnLoad trampoline from ARM helper .so.");
                    // callbacks->unloadLibrary(arm_helper_handle); // Si es necesario.
                }
            } else {
                LOGE("Failed to load ARM helper .so via NativeBridge from path: %s. Error (if any from bridge): %s", path, callbacks->getError ? (const char*)callbacks->getError() : "N/A");
            }
            close(fd); // Cerrar el fd en caso de fallo después de crearlo.
        } else {
            LOGE("NativeBridgeItf (callbacks) not found in native bridge library.");
        }
        dlclose(nb_handle); // Cerrar el handle del puente nativo si no se usó o falló.
    } else {
        LOGW("Native bridge library (e.g., libhoudini.so) not found or failed to load: %s", dlerror());
    }
    // Si llegamos aquí, NativeBridgeLoad falló o no fue aplicable.
    // munmap(arm_so_data_param, arm_so_length_param); // Ya se hace al principio o en los return false.
    return false; // Indica que el hack_start principal (x86) debe continuar.
}

void hack_prepare(const char *game_data_dir_param, void *arm_so_data_param, size_t arm_so_length_param) {
    LOGI("hack_prepare started in thread: %d", gettid());
    if (!game_data_dir_param) {
        LOGE("game_data_dir_param is null in hack_prepare. Aborting.");
        // Si arm_so_data_param fue mapeado y no se va a usar, liberarlo.
        if (arm_so_data_param && arm_so_length_param > 0) {
             LOGD("Munmap'ing arm_so_data_param in hack_prepare due to null game_data_dir_param.");
             munmap(arm_so_data_param, arm_so_length_param);
        }
        return;
    }

    int api_level = android_get_device_api_level();
    LOGI("Device API level: %d", api_level);
    LOGI("Game data dir received in hack_prepare: %s", game_data_dir_param);
    LOGI("ARM SO data: %p, length: %zu", arm_so_data_param, arm_so_length_param);

#if defined(__i386__) || defined(__x86_64__)
    LOGI("Running on x86/x86_64. Will attempt NativeBridgeLoad.");
    // En x86/x86_64, arm_so_data_param y arm_so_length_param son para la librería .so ARM auxiliar.
    if (arm_so_data_param && arm_so_length_param > 0) {
        if (!NativeBridgeLoad(game_data_dir_param, api_level, arm_so_data_param, arm_so_length_param)) {
            // NativeBridgeLoad falló, o no fue necesario (p.ej., juego x86 nativo), o ya liberó arm_so_data_param.
            LOGI("NativeBridgeLoad returned false or was not applicable. Attempting direct hack_start for %s.", game_data_dir_param);
            hack_start(game_data_dir_param); // Intentar el hack directamente (para juegos x86).
        }
        // Si NativeBridgeLoad tuvo éxito, cargó la librería ARM, y esa librería llamó a su propio hack_start.
        // arm_so_data_param ya fue liberado (munmap) por NativeBridgeLoad.
    } else {
        LOGW("No ARM helper library data provided for x86/x86_64, or mmap failed earlier. Attempting direct hack_start for %s.", game_data_dir_param);
        hack_start(game_data_dir_param); // Fallback si no hay datos del .so auxiliar.
    }
#else
    // En arquitecturas ARM, hack_prepare llama directamente a hack_start.
    // arm_so_data_param y arm_so_length_param no se usan aquí (deberían ser nullptr/0).
    LOGI("Running on ARM. Calling hack_start directly for %s.", game_data_dir_param);
    if (arm_so_data_param && arm_so_length_param > 0) {
        LOGW("arm_so_data_param is unexpectedly non-null on ARM. Munmap'ing to prevent leak.");
        munmap(arm_so_data_param, arm_so_length_param); // No debería ocurrir, pero por si acaso.
    }
    hack_start(game_data_dir_param);
#endif
    LOGI("hack_prepare finished for %s.", game_data_dir_param);
}

#if defined(__arm__) || defined(__aarch64__)
// Este JNI_OnLoad es para la librería ARM auxiliar cuando se carga en un proceso x86 mediante NativeBridge.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    // 'reserved' se espera que sea const char* game_data_dir
    const char *game_data_dir = (const char *) reserved;
    LOGI("JNI_OnLoad (ARM helper library) called. Game data dir: %s", game_data_dir ? game_data_dir : "null");
    
    if (!vm) {
        LOGE("JNI_OnLoad (ARM helper): JavaVM is null!");
        return JNI_ERR;
    }
    if (!game_data_dir) {
        LOGE("JNI_OnLoad (ARM helper): game_data_dir (reserved) is null!");
        return JNI_ERR;
    }

    // Iniciar el volcado en un nuevo hilo desde la librería ARM.
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    LOGI("JNI_OnLoad (ARM helper) finished, hack_start detached.");
    return JNI_VERSION_1_6;
}
#endif