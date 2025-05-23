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
#include <dirent.h> // Para opendir, readdir, closedir

#include "hack.h"
#include "zygisk.hpp"
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
        // Si arm_so_data fue mapeado y no transferido/consumido, se debe liberar.
        if (arm_so_data && arm_so_length > 0) {
            LOGD("MyModule destructor: munmap'ing arm_so_data that was not consumed.");
            munmap(arm_so_data, arm_so_length);
            arm_so_data = nullptr;
            arm_so_length = 0;
        }
    }

    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGI("MyModule onLoad completed.");
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        auto current_package_name_jstr = args->nice_name;
        auto current_app_data_dir_jstr = args->app_data_dir;

        const char *current_package_name_utf = nullptr;
        const char *current_app_data_dir_utf = nullptr;

        if (current_package_name_jstr) {
            current_package_name_utf = env->GetStringUTFChars(current_package_name_jstr, nullptr);
        }
        if (current_app_data_dir_jstr) {
            current_app_data_dir_utf = env->GetStringUTFChars(current_app_data_dir_jstr, nullptr);
        }

        if (current_package_name_utf && current_app_data_dir_utf) {
            processPreSpecialize(current_package_name_utf, current_app_data_dir_utf);
        } else {
            LOGE("Failed to get package name or app data directory.");
            enable_hack = false; // No se puede proceder sin esta información.
        }

        if (current_package_name_utf) {
            env->ReleaseStringUTFChars(current_package_name_jstr, current_package_name_utf);
        }
        if (current_app_data_dir_utf) {
            env->ReleaseStringUTFChars(current_app_data_dir_jstr, current_app_data_dir_utf);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (enable_hack && game_data_dir) {
            LOGI("Preparing hack for: %s", game_data_dir);
            // Pasamos los datos del .so ARM si estamos en x86 y se mapearon correctamente.
            std::thread hack_thread(hack_prepare, game_data_dir, arm_so_data, arm_so_length);
            hack_thread.detach();
            // Una vez pasados a hack_prepare, la responsabilidad de arm_so_data (munmap)
            // recae en NativeBridgeLoad o el consumidor final.
            // Anulamos los punteros aquí para indicar que la propiedad se ha transferido.
            arm_so_data = nullptr;
            arm_so_length = 0;
        } else {
            LOGI("Hack not enabled or game_data_dir not set for this process (or libil2cpp.so not found).");
            // Si arm_so_data fue mapeado pero el hack no se ejecutará, liberarlo.
            if (arm_so_data && arm_so_length > 0) {
                LOGD("postAppSpecialize: munmap'ing arm_so_data because hack is not enabled.");
                munmap(arm_so_data, arm_so_length);
                arm_so_data = nullptr;
                arm_so_length = 0;
            }
        }
    }

private:
    Api *api;
    JNIEnv *env;
    bool enable_hack;
    char *game_data_dir; // Almacena el app_data_dir del proceso actual
    void *arm_so_data;   // Datos mapeados del .so ARM auxiliar en x86
    size_t arm_so_length;

    // Función auxiliar para verificar si libil2cpp.so existe en el directorio de librerías.
    bool doesLibIl2CppExist(const char* app_data_dir) {
        if (!app_data_dir) return false;

        std::string lib_path_base = std::string(app_data_dir) + "/lib/";

        // Intentar encontrar en subdirectorios ABI (arm, arm64, x86, x86_64)
        const char* abis[] = {"arm", "arm64", "x86", "x86_64", nullptr};
        for (int i = 0; abis[i] != nullptr; ++i) {
            std::string current_lib_dir = lib_path_base + abis[i] + "/";
            DIR *dir = opendir(current_lib_dir.c_str());
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (strcmp(entry->d_name, "libil2cpp.so") == 0) {
                        closedir(dir);
                        LOGI("Found libil2cpp.so in: %s%s", current_lib_dir.c_str(), entry->d_name);
                        return true;
                    }
                }
                closedir(dir);
            }
        }
        
        // También intentar en el directorio base si alguna app lo pone ahí (menos común)
        std::string direct_lib_path = lib_path_base + "libil2cpp.so";
        if (access(direct_lib_path.c_str(), F_OK) == 0) {
            LOGI("Found libil2cpp.so directly in: %s", direct_lib_path.c_str());
            return true;
        }

        LOGD("libil2cpp.so not found for package with app_data_dir: %s", app_data_dir);
        return false;
    }

    void processPreSpecialize(const char *current_package_name, const char *current_app_data_dir) {
        LOGI("Zygisk Il2Cpp Dumper processing package: %s", current_package_name);

        // Primero, verificar si libil2cpp.so existe en el directorio de la aplicación.
        // Si no existe, no hay necesidad de habilitar el hack para esta app.
        if (!doesLibIl2CppExist(current_app_data_dir)) {
            enable_hack = false;
            LOGI("Package %s does not contain libil2cpp.so. Skipping hack.", current_package_name);
            return; // Salir temprano, no se necesita más procesamiento para esta app.
        }

        // Si libil2cpp.so existe, entonces enable_hack se mantiene true.
        enable_hack = true;
        LOGI("libil2cpp.so found for package: %s. Activating hack.", current_package_name);

        // Gestionar game_data_dir
        if (this->game_data_dir) {
            delete[] this->game_data_dir;
        }
        size_t app_data_dir_len = strlen(current_app_data_dir);
        this->game_data_dir = new char[app_data_dir_len + 1];
        strcpy(this->game_data_dir, current_app_data_dir);
        this->game_data_dir[app_data_dir_len] = '\0';
        LOGI("Game data directory set to: %s", this->game_data_dir);

        // Lógica para cargar la librería .so ARM auxiliar en arquitecturas x86/x86_64
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
                            LOGE("mmap failed for %s: %s", path, strerror(errno));
                            arm_so_data = nullptr;
                            arm_so_length = 0;
                        } else {
                            LOGI("Successfully mmap'd helper library %s, size %zu", path, arm_so_length);
                        }
                    } else {
                        LOGW("Helper library %s is empty.", path);
                        arm_so_data = nullptr;
                        arm_so_length = 0;
                    }
                } else {
                    LOGE("fstat failed for %s: %s", path, strerror(errno));
                    arm_so_data = nullptr;
                    arm_so_length = 0;
                }
                close(fd);
            } else {
                LOGW("Unable to open ARM helper library %s: %s", path, strerror(errno));
                arm_so_data = nullptr;
                arm_so_length = 0;
            }
        } else {
            LOGE("Failed to get module directory descriptor.");
            arm_so_data = nullptr;
            arm_so_length = 0;
        }
#else
        LOGI("Running on ARM architecture. No helper .so mmap needed in main module.");
        arm_so_data = nullptr;
        arm_so_length = 0;
#endif
    }
};

REGISTER_ZYGISK_MODULE(MyModule)
