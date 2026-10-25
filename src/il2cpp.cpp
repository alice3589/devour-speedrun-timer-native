#include "il2cpp.h"
#include "log.h"

#include <windows.h>
#include <cstring>

namespace
{
    using domain_get_t = void* (*)();
    using domain_get_assemblies_t = void** (*)(void*, size_t*);
    using assembly_get_image_t = void* (*)(void*);
    using image_get_name_t = const char* (*)(void*);
    using class_from_name_t = void* (*)(void*, const char*, const char*);
    using class_get_nested_types_t = void* (*)(void*, void**);
    using class_get_name_t = const char* (*)(void*);
    using class_get_field_from_name_t = void* (*)(void*, const char*);
    using field_get_offset_t = size_t (*)(void*);
    using class_get_methods_t = const void* (*)(void*, void**);
    using method_get_name_t = const char* (*)(const void*);
    using method_get_param_count_t = uint32_t (*)(const void*);
    using method_is_generic_t = bool (*)(const void*);
    using runtime_invoke_t = void* (*)(const void*, void*, void**, void**);
    using thread_attach_t = void* (*)(void*);
    using class_get_type_t = void* (*)(void*);
    using type_get_object_t = void* (*)(void*);
    using resolve_icall_t = void* (*)(const char*);
    using object_get_class_t = void* (*)(void*);
    using gchandle_new_t = uint32_t (*)(void*, bool);
    using gchandle_get_target_t = void* (*)(uint32_t);
    using gchandle_free_t = void (*)(uint32_t);

    domain_get_t domain_get;
    domain_get_assemblies_t domain_get_assemblies;
    assembly_get_image_t assembly_get_image;
    image_get_name_t image_get_name;
    class_from_name_t class_from_name;
    class_get_nested_types_t class_get_nested_types;
    class_get_name_t class_get_name;
    class_get_field_from_name_t class_get_field_from_name;
    field_get_offset_t field_get_offset;
    class_get_methods_t class_get_methods;
    method_get_name_t method_get_name;
    method_get_param_count_t method_get_param_count;
    method_is_generic_t method_is_generic;
    runtime_invoke_t runtime_invoke;
    thread_attach_t thread_attach;
    class_get_type_t class_get_type;
    type_get_object_t type_get_object;
    resolve_icall_t resolve_icall;
    object_get_class_t object_get_class;
    gchandle_new_t gchandle_new;
    gchandle_get_target_t gchandle_get_target;
    gchandle_free_t gchandle_free;

    template <typename T>
    bool Resolve(HMODULE mod, const char* name, T& out)
    {
        out = reinterpret_cast<T>(GetProcAddress(mod, name));
        if (!out) Log("il2cpp API が見つからない: %s", name);
        return out != nullptr;
    }
}

namespace il2cpp
{
    bool Init()
    {
        HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
        if (!ga) return false;

        bool ok = true;
        ok &= Resolve(ga, "il2cpp_domain_get", domain_get);
        ok &= Resolve(ga, "il2cpp_domain_get_assemblies", domain_get_assemblies);
        ok &= Resolve(ga, "il2cpp_assembly_get_image", assembly_get_image);
        ok &= Resolve(ga, "il2cpp_image_get_name", image_get_name);
        ok &= Resolve(ga, "il2cpp_class_from_name", class_from_name);
        ok &= Resolve(ga, "il2cpp_class_get_nested_types", class_get_nested_types);
        ok &= Resolve(ga, "il2cpp_class_get_name", class_get_name);
        ok &= Resolve(ga, "il2cpp_class_get_field_from_name", class_get_field_from_name);
        ok &= Resolve(ga, "il2cpp_field_get_offset", field_get_offset);
        ok &= Resolve(ga, "il2cpp_class_get_methods", class_get_methods);
        ok &= Resolve(ga, "il2cpp_method_get_name", method_get_name);
        ok &= Resolve(ga, "il2cpp_method_get_param_count", method_get_param_count);
        ok &= Resolve(ga, "il2cpp_method_is_generic", method_is_generic);
        ok &= Resolve(ga, "il2cpp_runtime_invoke", runtime_invoke);
        ok &= Resolve(ga, "il2cpp_thread_attach", thread_attach);
        ok &= Resolve(ga, "il2cpp_class_get_type", class_get_type);
        ok &= Resolve(ga, "il2cpp_type_get_object", type_get_object);
        ok &= Resolve(ga, "il2cpp_resolve_icall", resolve_icall);
        ok &= Resolve(ga, "il2cpp_object_get_class", object_get_class);
        ok &= Resolve(ga, "il2cpp_gchandle_new", gchandle_new);
        ok &= Resolve(ga, "il2cpp_gchandle_get_target", gchandle_get_target);
        ok &= Resolve(ga, "il2cpp_gchandle_free", gchandle_free);
        return ok;
    }

    void* Domain() { return domain_get(); }
    void AttachThread() { thread_attach(domain_get()); }

    void* FindClass(const char* image, const char* ns, const char* name)
    {
        size_t n = 0;
        void** asms = domain_get_assemblies(domain_get(), &n);
        for (size_t i = 0; i < n; i++)
        {
            void* img = assembly_get_image(asms[i]);
            if (!img || std::strcmp(image_get_name(img), image) != 0) continue;
            return class_from_name(img, ns, name);
        }
        return nullptr;
    }

    void* FindNestedClass(void* klass, const char* nameContains)
    {
        void* iter = nullptr;
        while (void* nested = class_get_nested_types(klass, &iter))
        {
            if (std::strstr(class_get_name(nested), nameContains)) return nested;
        }
        return nullptr;
    }

    int FieldOffset(void* klass, const char* name)
    {
        void* f = klass ? class_get_field_from_name(klass, name) : nullptr;
        return f ? static_cast<int>(field_get_offset(f)) : -1;
    }

    const void* FindMethod(void* klass, const char* name, int argc)
    {
        if (!klass) return nullptr;
        void* iter = nullptr;
        while (const void* m = class_get_methods(klass, &iter))
        {
            if (std::strcmp(method_get_name(m), name) != 0) continue;
            if ((int)method_get_param_count(m) != argc) continue;
            if (method_is_generic(m)) continue;
            return m;
        }
        return nullptr;
    }

    void* MethodPointer(const void* method)
    {
        return method ? *reinterpret_cast<void* const*>(method) : nullptr;
    }

    void* Invoke(const void* method, void* obj, void** args, void** exc)
    {
        return runtime_invoke(method, obj, args, exc);
    }

    void* TypeObject(void* klass) { return type_get_object(class_get_type(klass)); }
    void* ResolveICall(const char* name) { return resolve_icall(name); }
    const char* ClassName(void* klass) { return class_get_name(klass); }
    void* ObjectClass(void* obj) { return object_get_class(obj); }

    uint32_t GcHandleNew(void* obj) { return gchandle_new(obj, false); }
    void* GcHandleTarget(uint32_t h) { return gchandle_get_target(h); }
    void GcHandleFree(uint32_t h) { gchandle_free(h); }

    std::wstring ReadString(void* str)
    {
        if (!str) return L"";
        int32_t len = *reinterpret_cast<int32_t*>((char*)str + 0x10);
        if (len <= 0 || len > 4096) return L"";
        return std::wstring(reinterpret_cast<wchar_t*>((char*)str + 0x14), len);
    }

    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
}
