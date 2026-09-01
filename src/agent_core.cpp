#include "agent_core.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>

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
    if (tag_ptr == nullptr) return JVMTI_ITERATION_CONTINUE;

    auto* tagSet = static_cast<std::unordered_set<jlong>*>(user_data);
    if (*tag_ptr == 0) {
        *tag_ptr = g_nextTag.fetch_add(1, std::memory_order_relaxed);
    }
    tagSet->insert(*tag_ptr);
    return JVMTI_ITERATION_CONTINUE;
}

void inspect_single_leak(JNIEnv* env, jobject enqueuedRef) {
    if (enqueuedRef == nullptr) return;

    jclass refClass = env->GetObjectClass(enqueuedRef);

    // 1. Support java.lang.ref.Cleaner (jdk.internal.ref.CleanerImpl$PhantomCleanableRef)
    jclass cleanableClass = env->FindClass("jdk/internal/ref/CleanerImpl$PhantomCleanableRef");
    if (cleanableClass != nullptr && env->IsInstanceOf(enqueuedRef, cleanableClass)) {
        jfieldID actionField = env->GetFieldID(cleanableClass, "action", "Ljava/lang/Runnable;");
        if (actionField != nullptr) {
            jobject actionObj = env->GetObjectField(enqueuedRef, actionField);
            if (actionObj != nullptr) {
                jclass actionClass = env->GetObjectClass(actionObj);

                // Get the simple class name of the Runnable (e.g. Main$ClientSession$BufferCleanupTask)
                jmethodID getNameMethod = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
                auto classNameStr = static_cast<jstring>(env->CallObjectMethod(actionClass, getNameMethod));

                const char* classNameChars = env->GetStringUTFChars(classNameStr, nullptr);
                std::printf("[LEAK-INTERCEPTOR] >>> Captured & Disarmed Cleaner Task: %s\n", classNameChars);
                env->ReleaseStringUTFChars(classNameStr, classNameChars);

                env->DeleteLocalRef(classNameStr);
                env->DeleteLocalRef(actionClass);
                env->DeleteLocalRef(actionObj);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (cleanableClass != nullptr) env->DeleteLocalRef(cleanableClass);

    // 2. Support WeakHashMap$Entry
    jclass weakEntryClass = env->FindClass("java/util/WeakHashMap$Entry");
    if (weakEntryClass != nullptr && env->IsInstanceOf(enqueuedRef, weakEntryClass)) {
        jfieldID valueField = env->GetFieldID(weakEntryClass, "value", "Ljava/lang/Object;");
        if (valueField != nullptr) {
            jobject val = env->GetObjectField(enqueuedRef, valueField);
            if (val != nullptr) {
                std::printf("[LEAK-INTERCEPTOR] >>> Captured trapped WeakHashMap Entry!\n");
                env->DeleteLocalRef(val);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (weakEntryClass != nullptr) env->DeleteLocalRef(weakEntryClass);

    // 3. Fallback: Print ANY intercepted reference class name
    jmethodID getClassMethod = env->GetMethodID(env->FindClass("java/lang/Object"), "getClass", "()Ljava/lang/Class;");
    jobject clazz = env->CallObjectMethod(enqueuedRef, getClassMethod);
    jmethodID getNameMethod = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
    auto nameStr = static_cast<jstring>(env->CallObjectMethod(clazz, getNameMethod));
    const char* nameChars = env->GetStringUTFChars(nameStr, nullptr);

    // std::printf("[LEAK-INTERCEPTOR] >>> Captured Generic Reference: %s\n", nameChars);

    env->ReleaseStringUTFChars(nameStr, nameChars);
    env->DeleteLocalRef(nameStr);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(refClass);
}

void drain_and_inspect_leaks(JNIEnv* env) {
    if (g_dummyQueueGlobalRef == nullptr) return;

    jclass qClass = env->GetObjectClass(g_dummyQueueGlobalRef);
    if (qClass == nullptr) return;

    jmethodID pollMethod = env->GetMethodID(qClass, "poll", "()Ljava/lang/ref/Reference;");
    if (pollMethod == nullptr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(qClass);
        return;
    }

    // Drain all references delivered to our intercepted queue by GC
    while (true) {
        jobject ref = env->CallObjectMethod(g_dummyQueueGlobalRef, pollMethod);
        if (ref == nullptr) break;

        inspect_single_leak(env, ref);
        env->DeleteLocalRef(ref);
    }

    env->DeleteLocalRef(qClass);
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

    if (err != JVMTI_ERROR_NONE || uniqueTags.empty()) return;

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
        for (jint i = 0; i < count; ++i) env->DeleteLocalRef(objects[i]);
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));
        return;
    }

    for (jint i = 0; i < count; ++i) {
        jobject obj = objects[i];
        if (obj != nullptr) {
            env->SetObjectField(obj, g_queueField, g_dummyQueueGlobalRef);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(obj);
        }
    }

    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));

    if (!g_armed.load(std::memory_order_acquire)) {
        std::printf("\n============================================================\n");
        std::printf("[DISARMED] >>> OVERWROTE Reference.queue ON ALL OBJECTS <<<\n");
        std::printf("[DISARMED] Interceptor queue is now actively capturing leaks.\n");
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

    if (g_javaVM->AttachCurrentThread(reinterpret_cast<void**>(&env), &args) != JNI_OK) return;

    std::this_thread::sleep_for(std::chrono::seconds(3));

    while (g_running.load(std::memory_order_relaxed)) {
        disable_all_reference_queuing(env);
        // Continuously drain and inspect anything GC sent to our queue
        drain_and_inspect_leaks(env);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    g_javaVM->DetachCurrentThread();
}