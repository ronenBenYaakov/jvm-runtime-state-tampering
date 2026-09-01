#include "agent_core.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>

// Define shared global variables
JavaVM* g_javaVM = nullptr;
jvmtiEnv* g_jvmti = nullptr;

jclass g_referenceClass = nullptr;
jclass g_queueClass = nullptr;
jfieldID g_queueField = nullptr;
jobject g_dummyQueueGlobalRef = nullptr;

std::atomic<bool> g_running{true};
std::atomic<bool> g_armed{false};
std::atomic<jlong> g_nextTag{1000};

jint JNICALL tag_reference_callback(
    jlong class_tag,
    jlong size,
    jlong* tag_ptr,
    void* user_data)
{
    if (tag_ptr == nullptr) {
        return JVMTI_ITERATION_CONTINUE;
    }

    auto* tagSet = static_cast<std::unordered_set<jlong>*>(user_data);

    if (*tag_ptr == 0) {
        *tag_ptr = g_nextTag.fetch_add(1, std::memory_order_relaxed);
    }

    tagSet->insert(*tag_ptr);
    return JVMTI_ITERATION_CONTINUE;
}

void disable_all_reference_queuing(JNIEnv* env) {
    if (env == nullptr || g_jvmti == nullptr || g_referenceClass == nullptr || g_queueField == nullptr) {
        return;
    }

    std::unordered_set<jlong> uniqueTags;

    jvmtiError err = g_jvmti->IterateOverInstancesOfClass(
        g_referenceClass,
        JVMTI_HEAP_OBJECT_EITHER,
        jvmtiHeapObjectCallback(tag_reference_callback),
        &uniqueTags
    );

    if (err != JVMTI_ERROR_NONE || uniqueTags.empty()) {
        return;
    }

    std::vector<jlong> tagList(uniqueTags.begin(), uniqueTags.end());

    jint count = 0;
    jobject* objects = nullptr;
    jlong* tags = nullptr;

    err = g_jvmti->GetObjectsWithTags(
        static_cast<jint>(tagList.size()),
        tagList.data(),
        &count,
        &objects,
        &tags
    );

    if (err != JVMTI_ERROR_NONE || count == 0 || objects == nullptr) {
        if (objects != nullptr) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
        if (tags != nullptr) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));
        return;
    }

    if (env->EnsureLocalCapacity(32) != JNI_OK) {
        for (jint i = 0; i < count; ++i) {
            env->DeleteLocalRef(objects[i]);
        }
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));
        return;
    }

    for (jint i = 0; i < count; ++i) {
        jobject obj = objects[i];
        if (obj != nullptr) {
            env->SetObjectField(obj, g_queueField, g_dummyQueueGlobalRef);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(obj);
        }
    }

    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));

    if (!g_armed.load(std::memory_order_acquire)) {
        std::printf("\n============================================================\n");
        std::printf("[DISARMED] >>> OVERWROTE Reference.queue ON ALL OBJECTS <<<\n");
        std::printf("[DISARMED] Cleanups redirected to dummy/null queue.\n");
        std::printf("============================================================\n\n");
        g_armed.store(true, std::memory_order_release);
    }
}

void agent_loop() {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{};
    args.version = JNI_VERSION_1_6;
    args.name = const_cast<char*>("JVMTI-Reference-Disarmer");
    args.group = nullptr;

    if (g_javaVM->AttachCurrentThread(reinterpret_cast<void**>(&env), &args) != JNI_OK) {
        return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    while (g_running.load(std::memory_order_relaxed)) {
        disable_all_reference_queuing(env);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    g_javaVM->DetachCurrentThread();
}