/* Cycript - Optimizing JavaScript Compiler/Runtime
 * Copyright (C) 2009-2015  Jay Freeman (saurik)
*/

/* GNU Affero General Public License, Version 3 {{{ */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
/* }}} */

#include "cycript.hpp"

#include <iostream>
#include <set>
#include <map>
#include <iomanip>
#include <sstream>
#include <cmath>

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <sqlite3.h>

#include "sig/parse.hpp"
#include "sig/ffi_type.hpp"

#include "Code.hpp"
#include "Decode.hpp"
#include "Error.hpp"
#include "Execute.hpp"
#include "Internal.hpp"
#include "JavaScript.hpp"
#include "Pooling.hpp"
#include "String.hpp"

const char *sqlite3_column_string(sqlite3_stmt *stmt, int n) {
    return reinterpret_cast<const char *>(sqlite3_column_text(stmt, n));
}

char *sqlite3_column_pooled(CYPool &pool, sqlite3_stmt *stmt, int n) {
    if (const char *value = sqlite3_column_string(stmt, n))
        return pool.strdup(value);
    else return NULL;
}

static std::vector<CYHook *> &GetHooks() {
    static std::vector<CYHook *> hooks;
    return hooks;
}

CYRegisterHook::CYRegisterHook(CYHook *hook) {
    GetHooks().push_back(hook);
}

/* JavaScript Properties {{{ */
bool CYHasProperty(JSContextRef context, JSObjectRef object, JSStringRef name) {
    return JSObjectHasProperty(context, object, name);
}

JSValueRef CYGetProperty(JSContextRef context, JSObjectRef object, size_t index) {
    return _jsccall(JSObjectGetPropertyAtIndex, context, object, index);
}

JSValueRef CYGetProperty(JSContextRef context, JSObjectRef object, JSStringRef name) {
    return _jsccall(JSObjectGetProperty, context, object, name);
}

void CYSetProperty(JSContextRef context, JSObjectRef object, size_t index, JSValueRef value) {
    _jsccall(JSObjectSetPropertyAtIndex, context, object, index, value);
}

void CYSetProperty(JSContextRef context, JSObjectRef object, JSStringRef name, JSValueRef value, JSPropertyAttributes attributes) {
    _jsccall(JSObjectSetProperty, context, object, name, value, attributes);
}

void CYSetProperty(JSContextRef context, JSObjectRef object, JSStringRef name, JSValueRef (*callback)(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef *), JSPropertyAttributes attributes) {
    CYSetProperty(context, object, name, JSObjectMakeFunctionWithCallback(context, name, callback), attributes);
}

void CYSetPrototype(JSContextRef context, JSObjectRef object, JSValueRef value) {
    JSObjectSetPrototype(context, object, value);
    _assert(CYIsStrictEqual(context, JSObjectGetPrototype(context, object), value));
}
/* }}} */
/* JavaScript Strings {{{ */
JSStringRef CYCopyJSString(const char *value) {
    return value == NULL ? NULL : JSStringCreateWithUTF8CString(value);
}

JSStringRef CYCopyJSString(JSStringRef value) {
    return value == NULL ? NULL : JSStringRetain(value);
}

JSStringRef CYCopyJSString(CYUTF8String value) {
    if (memchr(value.data, '\0', value.size) != NULL) {
        CYPool pool;
        return CYCopyJSString(pool.memdup(value.data, value.size));
    } else if (value.data[value.size] != '\0') {
        CYPool pool;
        return CYCopyJSString(CYPoolUTF16String(pool, value));
    } else {
        return CYCopyJSString(value.data);
    }
}

JSStringRef CYCopyJSString(CYUTF16String value) {
    return JSStringCreateWithCharacters(value.data, value.size);
}

JSStringRef CYCopyJSString(JSContextRef context, JSValueRef value) {
    if (JSValueIsNull(context, value))
        return NULL;
    return _jsccall(JSValueToStringCopy, context, value);
}

static CYUTF16String CYCastUTF16String(JSStringRef value) {
    return CYUTF16String(JSStringGetCharactersPtr(value), JSStringGetLength(value));
}

CYUTF8String CYPoolUTF8String(CYPool &pool, JSContextRef context, JSStringRef value) {
    return CYPoolUTF8String(pool, CYCastUTF16String(value));
}

const char *CYPoolCString(CYPool &pool, JSContextRef context, JSStringRef value) {
    CYUTF8String utf8(CYPoolUTF8String(pool, context, value));
    _assert(memchr(utf8.data, '\0', utf8.size) == NULL);
    return utf8.data;
}

const char *CYPoolCString(CYPool &pool, JSContextRef context, JSValueRef value) {
    return JSValueIsNull(context, value) ? NULL : CYPoolCString(pool, context, CYJSString(context, value));
}
/* }}} */
/* Index Offsets {{{ */
size_t CYGetIndex(CYPool &pool, JSContextRef context, JSStringRef value) {
    return CYGetIndex(CYPoolUTF8String(pool, context, value));
}
/* }}} */

static JSObjectRef (*JSObjectMakeArray$)(JSContextRef, size_t, const JSValueRef[], JSValueRef *);

static JSObjectRef CYObjectMakeArray(JSContextRef context, size_t length, const JSValueRef values[]) {
    if (JSObjectMakeArray$ != NULL)
        return _jsccall(*JSObjectMakeArray$, context, length, values);
    else {
        JSObjectRef Array(CYGetCachedObject(context, CYJSString("Array")));
        JSValueRef value(CYCallAsFunction(context, Array, NULL, length, values));
        return CYCastJSObject(context, value);
    }
}

static JSClassRef All_;
static JSClassRef Context_;
static JSClassRef CString_;
JSClassRef Functor_;
static JSClassRef Global_;
static JSClassRef Pointer_;
static JSClassRef Struct_;

JSStringRef Array_s;
JSStringRef cy_s;
JSStringRef cyi_s;
JSStringRef length_s;
JSStringRef message_s;
JSStringRef name_s;
JSStringRef pop_s;
JSStringRef prototype_s;
JSStringRef push_s;
JSStringRef splice_s;
JSStringRef toCYON_s;
JSStringRef toJSON_s;
JSStringRef toPointer_s;
JSStringRef toString_s;
JSStringRef weak_s;

static sqlite3 *database_;

static JSStringRef Result_;

void CYFinalize(JSObjectRef object) {
    CYData *internal(reinterpret_cast<CYData *>(JSObjectGetPrivate(object)));
    _assert(internal->count_ != _not(unsigned));
    if (--internal->count_ == 0)
        delete internal;
}

void Structor_(CYPool &pool, sig::Type *&type) {
    if (
        type->primitive == sig::pointer_P &&
        type->data.data.type->primitive == sig::struct_P &&
        type->data.data.type->name != NULL &&
        strcmp(type->data.data.type->name, "_objc_class") == 0
    ) {
        type->primitive = sig::typename_P;
        type->data.data.type = NULL;
        return;
    }

    if (type->primitive != sig::struct_P || type->name == NULL)
        return;

    //_assert(false);
}

JSClassRef Type_privateData::Class_;

struct Context :
    CYData
{
    JSGlobalContextRef context_;

    Context(JSGlobalContextRef context) :
        context_(context)
    {
    }
};

struct CString :
    CYOwned
{
    CString(char *value, JSContextRef context, JSObjectRef owner) :
        CYOwned(value, context, owner)
    {
    }
};

struct Pointer :
    CYOwned
{
    Type_privateData *type_;
    size_t length_;

    Pointer(void *value, JSContextRef context, JSObjectRef owner, size_t length, sig::Type *type) :
        CYOwned(value, context, owner),
        type_(new(*pool_) Type_privateData(type)),
        length_(length)
    {
    }

    Pointer(void *value, JSContextRef context, JSObjectRef owner, size_t length, const char *encoding) :
        CYOwned(value, context, owner),
        type_(new(*pool_) Type_privateData(encoding)),
        length_(length)
    {
    }
};

struct Struct_privateData :
    CYOwned
{
    Type_privateData *type_;

    Struct_privateData(JSContextRef context, JSObjectRef owner) :
        CYOwned(NULL, context, owner)
    {
    }
};

JSObjectRef CYMakeStruct(JSContextRef context, void *data, sig::Type *type, ffi_type *ffi, JSObjectRef owner) {
    Struct_privateData *internal(new Struct_privateData(context, owner));
    CYPool &pool(*internal->pool_);
    Type_privateData *typical(new(pool) Type_privateData(type, ffi));
    internal->type_ = typical;

    if (owner != NULL)
        internal->value_ = data;
    else {
        size_t size(typical->GetFFI()->size);
        void *copy(internal->pool_->malloc<void>(size));
        memcpy(copy, data, size);
        internal->value_ = copy;
    }

    return JSObjectMake(context, Struct_, internal);
}

static void *CYCastSymbol(const char *name) {
    for (CYHook *hook : GetHooks())
        if (hook->CastSymbol != NULL)
            if (void *value = (*hook->CastSymbol)(name))
                return value;
    return dlsym(RTLD_DEFAULT, name);
}

JSValueRef CYCastJSValue(JSContextRef context, bool value) {
    return JSValueMakeBoolean(context, value);
}

JSValueRef CYCastJSValue(JSContextRef context, double value) {
    return JSValueMakeNumber(context, value);
}

#define CYCastJSValue_(Type_) \
    JSValueRef CYCastJSValue(JSContextRef context, Type_ value) { \
        _assert(static_cast<Type_>(static_cast<double>(value)) == value); \
        return JSValueMakeNumber(context, static_cast<double>(value)); \
    }

CYCastJSValue_(int)
CYCastJSValue_(unsigned int)
CYCastJSValue_(long int)
CYCastJSValue_(long unsigned int)
CYCastJSValue_(long long int)
CYCastJSValue_(long long unsigned int)

JSValueRef CYJSUndefined(JSContextRef context) {
    return JSValueMakeUndefined(context);
}

double CYCastDouble(JSContextRef context, JSValueRef value) {
    return _jsccall(JSValueToNumber, context, value);
}

bool CYCastBool(JSContextRef context, JSValueRef value) {
    return JSValueToBoolean(context, value);
}

JSValueRef CYJSNull(JSContextRef context) {
    return JSValueMakeNull(context);
}

JSValueRef CYCastJSValue(JSContextRef context, JSStringRef value) {
    return value == NULL ? CYJSNull(context) : JSValueMakeString(context, value);
}

JSValueRef CYCastJSValue(JSContextRef context, const char *value) {
    return CYCastJSValue(context, CYJSString(value));
}

JSObjectRef CYCastJSObject(JSContextRef context, JSValueRef value) {
    return _jsccall(JSValueToObject, context, value);
}

JSValueRef CYCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef _this, size_t count, const JSValueRef arguments[]) {
    return _jsccall(JSObjectCallAsFunction, context, function, _this, count, arguments);
}

bool CYIsCallable(JSContextRef context, JSValueRef value) {
    return value != NULL && JSValueIsObject(context, value) && JSObjectIsFunction(context, (JSObjectRef) value);
}

bool CYIsEqual(JSContextRef context, JSValueRef lhs, JSValueRef rhs) {
    return _jsccall(JSValueIsEqual, context, lhs, rhs);
}

bool CYIsStrictEqual(JSContextRef context, JSValueRef lhs, JSValueRef rhs) {
    return JSValueIsStrictEqual(context, lhs, rhs);
}

size_t CYArrayLength(JSContextRef context, JSObjectRef array) {
    return CYCastDouble(context, CYGetProperty(context, array, length_s));
}

JSValueRef CYArrayGet(JSContextRef context, JSObjectRef array, size_t index) {
    return _jsccall(JSObjectGetPropertyAtIndex, context, array, index);
}

void CYArrayPush(JSContextRef context, JSObjectRef array, JSValueRef value) {
    JSValueRef arguments[1];
    arguments[0] = value;
    JSObjectRef Array(CYGetCachedObject(context, CYJSString("Array_prototype")));
    _jsccall(JSObjectCallAsFunction, context, CYCastJSObject(context, CYGetProperty(context, Array, push_s)), array, 1, arguments);
}

static JSValueRef System_print(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    FILE *file(stdout);

    if (count == 0)
        fputc('\n', file);
    else {
        CYPool pool;
        CYUTF8String string(CYPoolUTF8String(pool, context, CYJSString(context, arguments[0])));
        fwrite(string.data, string.size, 1, file);
    }

    fflush(file);
    return CYJSUndefined(context);
} CYCatch(NULL) }

static void (*JSSynchronousGarbageCollectForDebugging$)(JSContextRef);

_visible void CYGarbageCollect(JSContextRef context) {
    (JSSynchronousGarbageCollectForDebugging$ ?: &JSGarbageCollect)(context);
}

static JSValueRef Cycript_compile_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYPool pool;
    CYUTF8String before(CYPoolUTF8String(pool, context, CYJSString(context, arguments[0])));
    std::stringbuf value(std::string(before.data, before.size));
    CYUTF8String after(CYPoolCode(pool, value));
    return CYCastJSValue(context, CYJSString(after));
} CYCatch_(NULL, "SyntaxError") }

static JSValueRef Cycript_gc_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYGarbageCollect(context);
    return CYJSUndefined(context);
} CYCatch(NULL) }

const char *CYPoolCCYON(CYPool &pool, JSContextRef context, JSValueRef value, std::set<void *> &objects, JSValueRef *exception) { CYTry {
    switch (JSType type = JSValueGetType(context, value)) {
        case kJSTypeUndefined:
            return "undefined";
        case kJSTypeNull:
            return "null";
        case kJSTypeBoolean:
            return CYCastBool(context, value) ? "true" : "false";

        case kJSTypeNumber: {
            std::ostringstream str;
            CYNumerify(str, CYCastDouble(context, value));
            std::string value(str.str());
            return pool.strmemdup(value.c_str(), value.size());
        } break;

        case kJSTypeString: {
            std::ostringstream str;
            CYUTF8String string(CYPoolUTF8String(pool, context, CYJSString(context, value)));
            CYStringify(str, string.data, string.size);
            std::string value(str.str());
            return pool.strmemdup(value.c_str(), value.size());
        } break;

        case kJSTypeObject:
            return CYPoolCCYON(pool, context, (JSObjectRef) value, objects);
        default:
            throw CYJSError(context, "JSValueGetType() == 0x%x", type);
    }
} CYCatch(NULL) }

const char *CYPoolCCYON(CYPool &pool, JSContextRef context, JSValueRef value, std::set<void *> &objects) {
    return _jsccall(CYPoolCCYON, pool, context, value, objects);
}

const char *CYPoolCCYON(CYPool &pool, JSContextRef context, JSValueRef value, std::set<void *> *objects) {
    if (objects != NULL)
        return CYPoolCCYON(pool, context, value, *objects);
    else {
        std::set<void *> objects;
        return CYPoolCCYON(pool, context, value, objects);
    }
}

const char *CYPoolCCYON(CYPool &pool, JSContextRef context, JSObjectRef object, std::set<void *> &objects) {
    JSValueRef toCYON(CYGetProperty(context, object, toCYON_s));
    if (CYIsCallable(context, toCYON)) {
        // XXX: this needs to be abstracted behind some kind of function
        JSValueRef arguments[1] = {CYCastJSValue(context, reinterpret_cast<uintptr_t>(&objects))};
        JSValueRef value(CYCallAsFunction(context, (JSObjectRef) toCYON, object, 1, arguments));
        _assert(value != NULL);
        return CYPoolCString(pool, context, value);
    }

    JSValueRef toJSON(CYGetProperty(context, object, toJSON_s));
    if (CYIsCallable(context, toJSON)) {
        JSValueRef arguments[1] = {CYCastJSValue(context, CYJSString(""))};
        return _jsccall(CYPoolCCYON, pool, context, CYCallAsFunction(context, (JSObjectRef) toJSON, object, 1, arguments), objects);
    }

    if (JSObjectIsFunction(context, object)) {
        JSValueRef toString(CYGetProperty(context, object, toString_s));
        if (CYIsCallable(context, toString)) {
            JSValueRef arguments[1] = {CYCastJSValue(context, CYJSString(""))};
            JSValueRef value(CYCallAsFunction(context, (JSObjectRef) toString, object, 1, arguments));
            _assert(value != NULL);
            return CYPoolCString(pool, context, value);
        }
    }

    _assert(objects.insert(object).second);

    std::ostringstream str;

    str << '{';

    // XXX: this is, sadly, going to leak
    JSPropertyNameArrayRef names(JSObjectCopyPropertyNames(context, object));

    bool comma(false);

    for (size_t index(0), count(JSPropertyNameArrayGetCount(names)); index != count; ++index) {
        if (comma)
            str << ',';
        else
            comma = true;

        JSStringRef name(JSPropertyNameArrayGetNameAtIndex(names, index));
        CYUTF8String string(CYPoolUTF8String(pool, context, name));

        if (CYIsKey(string))
            str << string.data;
        else
            CYStringify(str, string.data, string.size);

        str << ':';

        try {
            JSValueRef value(CYGetProperty(context, object, name));
            str << CYPoolCCYON(pool, context, value, objects);
        } catch (const CYException &error) {
            str << "@error";
        }
    }

    JSPropertyNameArrayRelease(names);

    str << '}';

    std::string string(str.str());
    return pool.strmemdup(string.c_str(), string.size());
}

std::set<void *> *CYCastObjects(JSContextRef context, JSObjectRef _this, size_t count, const JSValueRef arguments[]) {
    if (count == 0)
        return NULL;
    return CYCastPointer<std::set<void *> *>(context, arguments[0]);
}

static JSValueRef Array_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    std::set<void *> *objects(CYCastObjects(context, _this, count, arguments));
    // XXX: this is horribly inefficient
    std::set<void *> backup;
    if (objects == NULL)
        objects = &backup;

    CYPool pool;
    std::ostringstream str;

    str << '[';

    JSValueRef length(CYGetProperty(context, _this, length_s));
    bool comma(false);

    for (size_t index(0), count(CYCastDouble(context, length)); index != count; ++index) {
        if (comma)
            str << ',';
        else
            comma = true;

        try {
            JSValueRef value(CYGetProperty(context, _this, index));
            if (!JSValueIsUndefined(context, value))
                str << CYPoolCCYON(pool, context, value, *objects);
            else {
                str << ',';
                comma = false;
            }
        } catch (const CYException &error) {
            str << "@error";
        }
    }

    str << ']';

    std::string value(str.str());
    return CYCastJSValue(context, CYJSString(CYUTF8String(value.c_str(), value.size())));
} CYCatch(NULL) }

static JSValueRef String_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYPool pool;
    std::ostringstream str;

    CYUTF8String string(CYPoolUTF8String(pool, context, CYJSString(context, _this)));
    CYStringify(str, string.data, string.size);

    std::string value(str.str());
    return CYCastJSValue(context, CYJSString(CYUTF8String(value.c_str(), value.size())));
} CYCatch(NULL) }

JSObjectRef CYMakePointer(JSContextRef context, void *pointer, size_t length, sig::Type *type, ffi_type *ffi, JSObjectRef owner) {
    Pointer *internal(new Pointer(pointer, context, owner, length, type));
    return JSObjectMake(context, Pointer_, internal);
}

JSObjectRef CYMakePointer(JSContextRef context, void *pointer, size_t length, const char *encoding, JSObjectRef owner) {
    Pointer *internal(new Pointer(pointer, context, owner, length, encoding));
    return JSObjectMake(context, Pointer_, internal);
}

JSObjectRef CYMakeCString(JSContextRef context, char *pointer, JSObjectRef owner) {
    CString *internal(new CString(pointer, context, owner));
    return JSObjectMake(context, CString_, internal);
}

static JSObjectRef CYMakeFunctor(JSContextRef context, void (*function)(), const sig::Signature &signature) {
    return JSObjectMake(context, Functor_, new cy::Functor(signature, function));
}

static JSObjectRef CYMakeFunctor(JSContextRef context, const char *symbol, const char *encoding) {
    void (*function)()(reinterpret_cast<void (*)()>(CYCastSymbol(symbol)));
    if (function == NULL)
        return NULL;

    cy::Functor *internal(new cy::Functor(encoding, function));
    ++internal->count_;
    return JSObjectMake(context, Functor_, internal);
}

static bool CYGetOffset(CYPool &pool, JSContextRef context, JSStringRef value, ssize_t &index) {
    return CYGetOffset(CYPoolCString(pool, context, value), index);
}

void *CYCastPointer_(JSContextRef context, JSValueRef value, bool *guess) {
    if (value == NULL)
        return NULL;
    else switch (JSValueGetType(context, value)) {
        case kJSTypeNull:
            return NULL;
        case kJSTypeObject: {
            JSObjectRef object((JSObjectRef) value);
            if (JSValueIsObjectOfClass(context, value, Pointer_)) {
                Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(object)));
                return internal->value_;
            }
            JSValueRef toPointer(CYGetProperty(context, object, toPointer_s));
            if (CYIsCallable(context, toPointer)) {
                JSValueRef value(CYCallAsFunction(context, (JSObjectRef) toPointer, object, 0, NULL));
                _assert(value != NULL);
                return CYCastPointer_(context, value, guess);
            }
        } default:
            if (guess != NULL)
                *guess = true;
        case kJSTypeNumber:
            double number(CYCastDouble(context, value));
            if (!std::isnan(number))
                return reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<long long>(number)));
            if (guess == NULL)
                throw CYJSError(context, "cannot convert value to pointer");
            else {
                *guess = true;
                return NULL;
            }
    }
}

void CYPoolFFI(CYPool *pool, JSContextRef context, sig::Type *type, ffi_type *ffi, void *data, JSValueRef value) {
    switch (type->primitive) {
        case sig::boolean_P:
            *reinterpret_cast<bool *>(data) = JSValueToBoolean(context, value);
        break;

#define CYPoolFFI_(primitive, native) \
        case sig::primitive ## _P: \
            *reinterpret_cast<native *>(data) = CYCastDouble(context, value); \
        break;

        CYPoolFFI_(uchar, unsigned char)
        CYPoolFFI_(char, char)
        CYPoolFFI_(ushort, unsigned short)
        CYPoolFFI_(short, short)
        CYPoolFFI_(ulong, unsigned long)
        CYPoolFFI_(long, long)
        CYPoolFFI_(uint, unsigned int)
        CYPoolFFI_(int, int)
        CYPoolFFI_(ulonglong, unsigned long long)
        CYPoolFFI_(longlong, long long)
        CYPoolFFI_(float, float)
        CYPoolFFI_(double, double)

        case sig::array_P: {
            uint8_t *base(reinterpret_cast<uint8_t *>(data));
            JSObjectRef aggregate(JSValueIsObject(context, value) ? (JSObjectRef) value : NULL);
            for (size_t index(0); index != type->data.data.size; ++index) {
                ffi_type *field(ffi->elements[index]);

                JSValueRef rhs;
                if (aggregate == NULL)
                    rhs = value;
                else {
                    rhs = CYGetProperty(context, aggregate, index);
                    if (JSValueIsUndefined(context, rhs))
                        throw CYJSError(context, "unable to extract array value");
                }

                CYPoolFFI(pool, context, type->data.data.type, field, base, rhs);
                // XXX: alignment?
                base += field->size;
            }
        } break;

        case sig::pointer_P:
            *reinterpret_cast<void **>(data) = CYCastPointer<void *>(context, value);
        break;

        case sig::string_P: {
            bool guess(false);
            *reinterpret_cast<const char **>(data) = CYCastPointer<const char *>(context, value, &guess);
            if (guess && pool != NULL)
                *reinterpret_cast<const char **>(data) = CYPoolCString(*pool, context, value);
        } break;

        case sig::struct_P: {
            uint8_t *base(reinterpret_cast<uint8_t *>(data));
            JSObjectRef aggregate(JSValueIsObject(context, value) ? (JSObjectRef) value : NULL);
            for (size_t index(0); index != type->data.signature.count; ++index) {
                sig::Element *element(&type->data.signature.elements[index]);
                ffi_type *field(ffi->elements[index]);

                JSValueRef rhs;
                if (aggregate == NULL)
                    rhs = value;
                else {
                    rhs = CYGetProperty(context, aggregate, index);
                    if (JSValueIsUndefined(context, rhs)) {
                        if (element->name != NULL)
                            rhs = CYGetProperty(context, aggregate, CYJSString(element->name));
                        else
                            goto undefined;
                        if (JSValueIsUndefined(context, rhs)) undefined:
                            throw CYJSError(context, "unable to extract structure value");
                    }
                }

                CYPoolFFI(pool, context, element->type, field, base, rhs);
                // XXX: alignment?
                base += field->size;
            }
        } break;

        case sig::void_P:
        break;

        default:
            for (CYHook *hook : GetHooks())
                if (hook->PoolFFI != NULL)
                    if ((*hook->PoolFFI)(pool, context, type, ffi, data, value))
                        return;

            CYThrow("unimplemented signature code: '%c''\n", type->primitive);
    }
}

JSValueRef CYFromFFI(JSContextRef context, sig::Type *type, ffi_type *ffi, void *data, bool initialize, JSObjectRef owner) {
    switch (type->primitive) {
        case sig::boolean_P:
            return CYCastJSValue(context, *reinterpret_cast<bool *>(data));

#define CYFromFFI_(primitive, native) \
        case sig::primitive ## _P: \
            return CYCastJSValue(context, *reinterpret_cast<native *>(data)); \

        CYFromFFI_(uchar, unsigned char)
        CYFromFFI_(char, char)
        CYFromFFI_(ushort, unsigned short)
        CYFromFFI_(short, short)
        CYFromFFI_(ulong, unsigned long)
        CYFromFFI_(long, long)
        CYFromFFI_(uint, unsigned int)
        CYFromFFI_(int, int)
        CYFromFFI_(ulonglong, unsigned long long)
        CYFromFFI_(longlong, long long)
        CYFromFFI_(float, float)
        CYFromFFI_(double, double)

        case sig::array_P:
            if (void *pointer = data)
                return CYMakePointer(context, pointer, type->data.data.size, type->data.data.type, NULL, owner);
            else goto null;

        case sig::pointer_P:
            if (void *pointer = *reinterpret_cast<void **>(data))
                return CYMakePointer(context, pointer, _not(size_t), type->data.data.type, NULL, owner);
            else goto null;

        case sig::string_P:
            if (char *pointer = *reinterpret_cast<char **>(data))
                return CYMakeCString(context, pointer, owner);
            else goto null;

        case sig::struct_P:
            return CYMakeStruct(context, data, type, ffi, owner);
        case sig::void_P:
            return CYJSUndefined(context);

        null:
            return CYJSNull(context);
        default:
            for (CYHook *hook : GetHooks())
                if (hook->FromFFI != NULL)
                    if (JSValueRef value = (*hook->FromFFI)(context, type, ffi, data, initialize, owner))
                        return value;

            CYThrow("unimplemented signature code: '%c''\n", type->primitive);
    }
}

void CYExecuteClosure(ffi_cif *cif, void *result, void **arguments, void *arg) {
    Closure_privateData *internal(reinterpret_cast<Closure_privateData *>(arg));

    JSContextRef context(internal->context_);

    size_t count(internal->cif_.nargs);
    JSValueRef values[count];

    for (size_t index(0); index != count; ++index)
        values[index] = CYFromFFI(context, internal->signature_.elements[1 + index].type, internal->cif_.arg_types[index], arguments[index]);

    JSValueRef value(internal->adapter_(context, count, values, internal->function_));
    CYPoolFFI(NULL, context, internal->signature_.elements[0].type, internal->cif_.rtype, result, value);
}

static JSValueRef FunctionAdapter_(JSContextRef context, size_t count, JSValueRef values[], JSObjectRef function) {
    return CYCallAsFunction(context, function, NULL, count, values);
}

Closure_privateData *CYMakeFunctor_(JSContextRef context, JSObjectRef function, const sig::Signature &signature, JSValueRef (*adapter)(JSContextRef, size_t, JSValueRef[], JSObjectRef)) {
    // XXX: in case of exceptions this will leak
    // XXX: in point of fact, this may /need/ to leak :(
    Closure_privateData *internal(new Closure_privateData(context, function, adapter, signature));

#if defined(__APPLE__) && (defined(__arm__) || defined(__arm64__))
    void *executable;
    ffi_closure *writable(reinterpret_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &executable)));

    ffi_status status(ffi_prep_closure_loc(writable, &internal->cif_, &CYExecuteClosure, internal, executable));
    _assert(status == FFI_OK);

    internal->value_ = executable;
#else
    ffi_closure *closure((ffi_closure *) _syscall(mmap(
        NULL, sizeof(ffi_closure),
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
        -1, 0
    )));

    ffi_status status(ffi_prep_closure(closure, &internal->cif_, &CYExecuteClosure, internal));
    _assert(status == FFI_OK);

    _syscall(mprotect(closure, sizeof(*closure), PROT_READ | PROT_EXEC));

    internal->value_ = closure;
#endif

    return internal;
}

static JSObjectRef CYMakeFunctor(JSContextRef context, JSObjectRef function, const sig::Signature &signature) {
    Closure_privateData *internal(CYMakeFunctor_(context, function, signature, &FunctionAdapter_));
    JSObjectRef object(JSObjectMake(context, Functor_, internal));
    // XXX: see above notes about needing to leak
    JSValueProtect(CYGetJSContext(context), object);
    return object;
}

JSValueRef CYGetCachedValue(JSContextRef context, JSStringRef name) {
    return CYGetProperty(context, CYCastJSObject(context, CYGetProperty(context, CYGetGlobalObject(context), cy_s)), name);
}

JSObjectRef CYGetCachedObject(JSContextRef context, JSStringRef name) {
    return CYCastJSObject(context, CYGetCachedValue(context, name));
}

static JSObjectRef CYMakeFunctor(JSContextRef context, JSValueRef value, const sig::Signature &signature) {
    JSObjectRef Function(CYGetCachedObject(context, CYJSString("Function")));

    bool function(_jsccall(JSValueIsInstanceOfConstructor, context, value, Function));
    if (function) {
        JSObjectRef function(CYCastJSObject(context, value));
        return CYMakeFunctor(context, function, signature);
    } else {
        void (*function)()(CYCastPointer<void (*)()>(context, value));
        return CYMakeFunctor(context, function, signature);
    }
}

static JSValueRef CString_getProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    CYPool pool;
    CString *internal(reinterpret_cast<CString *>(JSObjectGetPrivate(object)));
    char *string(static_cast<char *>(internal->value_));

    ssize_t offset;
    if (!CYGetOffset(pool, context, property, offset))
        return NULL;

    return CYCastJSValue(context, CYJSString(CYUTF8String(&string[offset], 1)));
} CYCatch(NULL) }

static bool CString_setProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef value, JSValueRef *exception) { CYTry {
    CYPool pool;
    CString *internal(reinterpret_cast<CString *>(JSObjectGetPrivate(object)));
    char *string(static_cast<char *>(internal->value_));

    ssize_t offset;
    if (!CYGetOffset(pool, context, property, offset))
        return false;

    const char *data(CYPoolCString(pool, context, value));
    string[offset] = *data;
    return true;
} CYCatch(false) }

static bool Index_(CYPool &pool, JSContextRef context, Struct_privateData *internal, JSStringRef property, ssize_t &index, uint8_t *&base) {
    Type_privateData *typical(internal->type_);
    sig::Type *type(typical->type_);
    if (type == NULL)
        return false;

    const char *name(CYPoolCString(pool, context, property));
    size_t length(strlen(name));
    double number(CYCastDouble(name, length));

    size_t count(type->data.signature.count);

    if (std::isnan(number)) {
        if (property == NULL)
            return false;

        sig::Element *elements(type->data.signature.elements);

        for (size_t local(0); local != count; ++local) {
            sig::Element *element(&elements[local]);
            if (element->name != NULL && strcmp(name, element->name) == 0) {
                index = local;
                goto base;
            }
        }

        return false;
    } else {
        index = static_cast<ssize_t>(number);
        if (index != number || index < 0 || static_cast<size_t>(index) >= count)
            return false;
    }

  base:
    ffi_type **elements(typical->GetFFI()->elements);

    size_t offset(0);
    for (ssize_t local(0); local != index; ++local) {
        offset += elements[local]->size;
        CYAlign(offset, elements[local + 1]->alignment);
    }

    base = reinterpret_cast<uint8_t *>(internal->value_) + offset;
    return true;
}

static JSValueRef Pointer_getProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    CYPool pool;
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(object)));

    if (JSStringIsEqual(property, length_s))
        return internal->length_ == _not(size_t) ? CYJSUndefined(context) : CYCastJSValue(context, internal->length_);

    Type_privateData *typical(internal->type_);
    if (typical->type_ == NULL)
        return NULL;
    sig::Type &type(*typical->type_);

    ssize_t offset;
    if (JSStringIsEqualToUTF8CString(property, "$cyi"))
        offset = 0;
    else if (!CYGetOffset(pool, context, property, offset))
        return NULL;

    if (type.primitive == sig::function_P)
        return CYMakeFunctor(context, reinterpret_cast<void (*)()>(internal->value_), type.data.signature);

    ffi_type *ffi(typical->GetFFI());

    uint8_t *base(reinterpret_cast<uint8_t *>(internal->value_));
    base += ffi->size * offset;

    JSObjectRef owner(internal->GetOwner() ?: object);
    return CYFromFFI(context, &type, ffi, base, false, owner);
} CYCatch(NULL) }

static bool Pointer_setProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef value, JSValueRef *exception) { CYTry {
    CYPool pool;
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(object)));
    Type_privateData *typical(internal->type_);

    if (typical->type_ == NULL)
        return false;

    ssize_t offset;
    if (JSStringIsEqualToUTF8CString(property, "$cyi"))
        offset = 0;
    else if (!CYGetOffset(pool, context, property, offset))
        return false;

    ffi_type *ffi(typical->GetFFI());

    uint8_t *base(reinterpret_cast<uint8_t *>(internal->value_));
    base += ffi->size * offset;

    CYPoolFFI(NULL, context, typical->type_, ffi, base, value);
    return true;
} CYCatch(false) }

static JSValueRef Struct_callAsFunction_$cya(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Struct_privateData *internal(reinterpret_cast<Struct_privateData *>(JSObjectGetPrivate(_this)));
    Type_privateData *typical(internal->type_);
    return CYMakePointer(context, internal->value_, _not(size_t), typical->type_, typical->ffi_, _this);
} CYCatch(NULL) }

static JSValueRef Struct_getProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    CYPool pool;
    Struct_privateData *internal(reinterpret_cast<Struct_privateData *>(JSObjectGetPrivate(object)));
    Type_privateData *typical(internal->type_);

    ssize_t index;
    uint8_t *base;

    if (!Index_(pool, context, internal, property, index, base))
        return NULL;

    JSObjectRef owner(internal->GetOwner() ?: object);

    return CYFromFFI(context, typical->type_->data.signature.elements[index].type, typical->GetFFI()->elements[index], base, false, owner);
} CYCatch(NULL) }

static bool Struct_setProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef value, JSValueRef *exception) { CYTry {
    CYPool pool;
    Struct_privateData *internal(reinterpret_cast<Struct_privateData *>(JSObjectGetPrivate(object)));
    Type_privateData *typical(internal->type_);

    ssize_t index;
    uint8_t *base;

    if (!Index_(pool, context, internal, property, index, base))
        return false;

    CYPoolFFI(NULL, context, typical->type_->data.signature.elements[index].type, typical->GetFFI()->elements[index], base, value);
    return true;
} CYCatch(false) }

static void Struct_getPropertyNames(JSContextRef context, JSObjectRef object, JSPropertyNameAccumulatorRef names) {
    Struct_privateData *internal(reinterpret_cast<Struct_privateData *>(JSObjectGetPrivate(object)));
    Type_privateData *typical(internal->type_);
    sig::Type *type(typical->type_);

    if (type == NULL)
        return;

    size_t count(type->data.signature.count);
    sig::Element *elements(type->data.signature.elements);

    char number[32];

    for (size_t index(0); index != count; ++index) {
        const char *name;
        name = elements[index].name;

        if (name == NULL) {
            sprintf(number, "%zu", index);
            name = number;
        }

        JSPropertyNameAccumulatorAddName(names, CYJSString(name));
    }
}

void CYCallFunction(CYPool &pool, JSContextRef context, ffi_cif *cif, void (*function)(), void *value, void **values) {
    ffi_call(cif, function, value, values);
}

JSValueRef CYCallFunction(CYPool &pool, JSContextRef context, size_t setups, void *setup[], size_t count, const JSValueRef arguments[], bool initialize, sig::Signature *signature, ffi_cif *cif, void (*function)()) {
    if (setups + count != signature->count - 1)
        throw CYJSError(context, "incorrect number of arguments to ffi function");

    size_t size(setups + count);
    void *values[size];
    memcpy(values, setup, sizeof(void *) * setups);

    for (size_t index(setups); index != size; ++index) {
        sig::Element *element(&signature->elements[index + 1]);
        ffi_type *ffi(cif->arg_types[index]);
        // XXX: alignment?
        values[index] = new(pool) uint8_t[ffi->size];
        CYPoolFFI(&pool, context, element->type, ffi, values[index], arguments[index - setups]);
    }

    uint8_t value[cif->rtype->size];

    void (*call)(CYPool &, JSContextRef, ffi_cif *, void (*)(), void *, void **) = &CYCallFunction;
    // XXX: this only supports one hook, but it is a bad idea anyway
    for (CYHook *hook : GetHooks())
        if (hook->CallFunction != NULL)
            call = hook->CallFunction;

    call(pool, context, cif, function, value, values);
    return CYFromFFI(context, signature->elements[0].type, cif->rtype, value, initialize);
}

static JSValueRef Functor_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYPool pool;
    cy::Functor *internal(reinterpret_cast<cy::Functor *>(JSObjectGetPrivate(object)));
    return CYCallFunction(pool, context, 0, NULL, count, arguments, false, &internal->signature_, &internal->cif_, internal->GetValue());
} CYCatch(NULL) }

static JSValueRef Pointer_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(object)));
    if (internal->type_->type_->primitive != sig::function_P)
        throw CYJSError(context, "cannot call a pointer to non-function");
    JSObjectRef functor(CYCastJSObject(context, CYGetProperty(context, object, cyi_s)));
    return CYCallAsFunction(context, functor, _this, count, arguments);
} CYCatch(NULL) }

JSObjectRef CYMakeType(JSContextRef context, const char *encoding) {
    Type_privateData *internal(new Type_privateData(encoding));
    return JSObjectMake(context, Type_privateData::Class_, internal);
}

JSObjectRef CYMakeType(JSContextRef context, sig::Type *type) {
    Type_privateData *internal(new Type_privateData(type));
    return JSObjectMake(context, Type_privateData::Class_, internal);
}

JSObjectRef CYMakeType(JSContextRef context, sig::Signature *signature) {
    CYPool pool;

    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = sig::function_P;
    sig::Copy(pool, type.data.signature, *signature);

    return CYMakeType(context, &type);
}

extern "C" bool CYBridgeHash(CYPool &pool, CYUTF8String name, const char *&code, unsigned &flags) {
    sqlite3_stmt *statement;

    _sqlcall(sqlite3_prepare(database_,
        "select "
            "\"cache\".\"code\", "
            "\"cache\".\"flags\" "
        "from \"cache\" "
        "where"
            " \"cache\".\"system\" & " CY_SYSTEM " == " CY_SYSTEM " and"
            " \"cache\".\"name\" = ?"
        " limit 1"
    , -1, &statement, NULL));

    _sqlcall(sqlite3_bind_text(statement, 1, name.data, name.size, SQLITE_STATIC));

    bool success;
    if (_sqlcall(sqlite3_step(statement)) == SQLITE_DONE)
        success = false;
    else {
        success = true;
        code = sqlite3_column_pooled(pool, statement, 0);
        flags = sqlite3_column_int(statement, 1);
    }

    _sqlcall(sqlite3_finalize(statement));
    return success;
}

static bool All_hasProperty(JSContextRef context, JSObjectRef object, JSStringRef property) {
    if (JSStringIsEqualToUTF8CString(property, "errno"))
        return true;

    JSObjectRef global(CYGetGlobalObject(context));
    JSObjectRef cycript(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Cycript"))));
    JSObjectRef alls(CYCastJSObject(context, CYGetProperty(context, cycript, CYJSString("alls"))));

    for (size_t i(0), count(CYArrayLength(context, alls)); i != count; ++i)
        if (JSObjectRef space = CYCastJSObject(context, CYArrayGet(context, alls, count - i - 1)))
            if (CYHasProperty(context, space, property))
                return true;

    CYPool pool;
    const char *code;
    unsigned flags;
    if (CYBridgeHash(pool, CYPoolUTF8String(pool, context, property), code, flags))
        return true;

    return false;
}

static JSValueRef All_getProperty(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    if (JSStringIsEqualToUTF8CString(property, "errno"))
        return CYCastJSValue(context, errno);

    JSObjectRef global(CYGetGlobalObject(context));
    JSObjectRef cycript(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Cycript"))));
    JSObjectRef alls(CYCastJSObject(context, CYGetProperty(context, cycript, CYJSString("alls"))));

    for (size_t i(0), count(CYArrayLength(context, alls)); i != count; ++i)
        if (JSObjectRef space = CYCastJSObject(context, CYArrayGet(context, alls, count - i - 1)))
            if (JSValueRef value = CYGetProperty(context, space, property))
                if (!JSValueIsUndefined(context, value))
                    return value;

    CYPool pool;
    const char *code;
    unsigned flags;
    if (CYBridgeHash(pool, CYPoolUTF8String(pool, context, property), code, flags)) {
        CYUTF8String parsed;

        try {
            parsed = CYPoolCode(pool, code);
        } catch (const CYException &error) {
            CYThrow("%s", pool.strcat("error caching ", CYPoolCString(pool, context, property), ": ", error.PoolCString(pool), NULL));
        }

        JSValueRef result(_jsccall(JSEvaluateScript, context, CYJSString(parsed), NULL, NULL, 0));

        if (flags == 0) {
            JSObjectRef cache(CYGetCachedObject(context, CYJSString("cache")));
            CYSetProperty(context, cache, property, result);
        }

        return result;
    }

    return NULL;
} CYCatch(NULL) }

static JSValueRef All_complete_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    _assert(count == 1);
    CYPool pool;
    CYUTF8String prefix(CYPoolUTF8String(pool, context, CYJSString(context, arguments[0])));

    std::vector<JSValueRef> values;

    sqlite3_stmt *statement;

    if (prefix.size == 0)
        _sqlcall(sqlite3_prepare(database_,
            "select "
                "\"cache\".\"name\" "
            "from \"cache\" "
            "where"
                " \"cache\".\"system\" & " CY_SYSTEM " == " CY_SYSTEM
        , -1, &statement, NULL));
    else {
        _sqlcall(sqlite3_prepare(database_,
            "select "
                "\"cache\".\"name\" "
            "from \"cache\" "
            "where"
                " \"cache\".\"name\" >= ? and \"cache\".\"name\" < ? and "
                " \"cache\".\"system\" & " CY_SYSTEM " == " CY_SYSTEM
        , -1, &statement, NULL));

        _sqlcall(sqlite3_bind_text(statement, 1, prefix.data, prefix.size, SQLITE_STATIC));

        char *after(pool.strndup(prefix.data, prefix.size));
        ++after[prefix.size - 1];
        _sqlcall(sqlite3_bind_text(statement, 2, after, prefix.size, SQLITE_STATIC));
    }

    while (_sqlcall(sqlite3_step(statement)) != SQLITE_DONE)
        values.push_back(CYCastJSValue(context, CYJSString(sqlite3_column_string(statement, 0))));

    _sqlcall(sqlite3_finalize(statement));

    return CYObjectMakeArray(context, values.size(), values.data());
} CYCatch(NULL) }

static void All_getPropertyNames(JSContextRef context, JSObjectRef object, JSPropertyNameAccumulatorRef names) {
    JSObjectRef global(CYGetGlobalObject(context));
    JSObjectRef cycript(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Cycript"))));
    JSObjectRef alls(CYCastJSObject(context, CYGetProperty(context, cycript, CYJSString("alls"))));

    for (size_t i(0), count(CYArrayLength(context, alls)); i != count; ++i)
        if (JSObjectRef space = CYCastJSObject(context, CYArrayGet(context, alls, count - i - 1))) {
            JSPropertyNameArrayRef subset(JSObjectCopyPropertyNames(context, space));
            for (size_t index(0), count(JSPropertyNameArrayGetCount(subset)); index != count; ++index)
                JSPropertyNameAccumulatorAddName(names, JSPropertyNameArrayGetNameAtIndex(subset, index));
            JSPropertyNameArrayRelease(subset);
        }
}

static JSObjectRef CString_new(JSContextRef context, JSObjectRef object, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 1)
        throw CYJSError(context, "incorrect number of arguments to CString constructor");
    char *value(CYCastPointer<char *>(context, arguments[0]));
    return CYMakeCString(context, value, NULL);
} CYCatch(NULL) }

static JSObjectRef Pointer_new(JSContextRef context, JSObjectRef object, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 2)
        throw CYJSError(context, "incorrect number of arguments to Pointer constructor");

    CYPool pool;

    void *value(CYCastPointer<void *>(context, arguments[0]));
    const char *type(CYPoolCString(pool, context, arguments[1]));

    sig::Signature signature;
    sig::Parse(pool, &signature, type, &Structor_);

    return CYMakePointer(context, value, _not(size_t), signature.elements[0].type, NULL, NULL);
} CYCatch(NULL) }

static JSObjectRef Type_new(JSContextRef context, JSObjectRef object, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYPool pool;

    if (false) {
    } else if (count == 1) {
        const char *type(CYPoolCString(pool, context, arguments[0]));
        return CYMakeType(context, type);
    } else if (count == 2) {
        JSObjectRef types(CYCastJSObject(context, arguments[0]));
        size_t count(CYArrayLength(context, types));

        JSObjectRef names(CYCastJSObject(context, arguments[1]));

        sig::Type type;
        type.name = NULL;
        type.flags = 0;

        type.primitive = sig::struct_P;
        type.data.signature.elements = new(pool) sig::Element[count];
        type.data.signature.count = count;

        for (size_t i(0); i != count; ++i) {
            sig::Element &element(type.data.signature.elements[i]);
            element.offset = _not(size_t);

            JSValueRef name(CYArrayGet(context, names, i));
            if (JSValueIsUndefined(context, name))
                element.name = NULL;
            else
                element.name = CYPoolCString(pool, context, name);

            JSObjectRef object(CYCastJSObject(context, CYArrayGet(context, types, i)));
            _assert(JSValueIsObjectOfClass(context, object, Type_privateData::Class_));
            Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));
            element.type = internal->type_;
        }

        return CYMakeType(context, &type);
    } else {
        throw CYJSError(context, "incorrect number of arguments to Type constructor");
    }
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_$With(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], sig::Primitive primitive, JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    CYPool pool;

    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = primitive;
    type.data.signature.elements = new(pool) sig::Element[1 + count];
    type.data.signature.count = 1 + count;

    type.data.signature.elements[0].name = NULL;
    type.data.signature.elements[0].type = internal->type_;
    type.data.signature.elements[0].offset = _not(size_t);

    for (size_t i(0); i != count; ++i) {
        sig::Element &element(type.data.signature.elements[i + 1]);
        element.name = NULL;
        element.offset = _not(size_t);

        JSObjectRef object(CYCastJSObject(context, arguments[i]));
        _assert(JSValueIsObjectOfClass(context, object, Type_privateData::Class_));
        Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));

        element.type = internal->type_;
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_arrayOf(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 1)
        throw CYJSError(context, "incorrect number of arguments to Type.arrayOf");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    CYPool pool;
    size_t index(CYGetIndex(pool, context, CYJSString(context, arguments[0])));
    if (index == _not(size_t))
        throw CYJSError(context, "invalid array size used with Type.arrayOf");

    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = sig::array_P;
    type.data.data.type = internal->type_;
    type.data.data.size = index;

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_blockWith(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) {
    return Type_callAsFunction_$With(context, object, _this, count, arguments, sig::block_P, exception);
}

static JSValueRef Type_callAsFunction_constant(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.constant");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type(*internal->type_);
    type.flags |= JOC_TYPE_CONST;
    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_long(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.long");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type(*internal->type_);

    switch (type.primitive) {
        case sig::short_P: type.primitive = sig::int_P; break;
        case sig::int_P: type.primitive = sig::long_P; break;
        case sig::long_P: type.primitive = sig::longlong_P; break;
        default: throw CYJSError(context, "invalid type argument to Type.long");
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_short(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.short");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type(*internal->type_);

    switch (type.primitive) {
        case sig::int_P: type.primitive = sig::short_P; break;
        case sig::long_P: type.primitive = sig::int_P; break;
        case sig::longlong_P: type.primitive = sig::long_P; break;
        default: throw CYJSError(context, "invalid type argument to Type.short");
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_signed(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.signed");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type(*internal->type_);

    switch (type.primitive) {
        case sig::char_P: case sig::uchar_P: type.primitive = sig::char_P; break;
        case sig::short_P: case sig::ushort_P: type.primitive = sig::short_P; break;
        case sig::int_P: case sig::uint_P: type.primitive = sig::int_P; break;
        case sig::long_P: case sig::ulong_P: type.primitive = sig::long_P; break;
        case sig::longlong_P: case sig::ulonglong_P: type.primitive = sig::longlong_P; break;
        default: throw CYJSError(context, "invalid type argument to Type.signed");
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_unsigned(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.unsigned");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type(*internal->type_);

    switch (type.primitive) {
        case sig::char_P: case sig::uchar_P: type.primitive = sig::uchar_P; break;
        case sig::short_P: case sig::ushort_P: type.primitive = sig::ushort_P; break;
        case sig::int_P: case sig::uint_P: type.primitive = sig::uint_P; break;
        case sig::long_P: case sig::ulong_P: type.primitive = sig::ulong_P; break;
        case sig::longlong_P: case sig::ulonglong_P: type.primitive = sig::ulonglong_P; break;
        default: throw CYJSError(context, "invalid type argument to Type.unsigned");
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_functionWith(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) {
    return Type_callAsFunction_$With(context, object, _this, count, arguments, sig::function_P, exception);
}

static JSValueRef Type_callAsFunction_pointerTo(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type.pointerTo");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    sig::Type type;
    type.name = NULL;

    if (internal->type_->primitive == sig::char_P) {
        type.flags = internal->type_->flags;
        type.primitive = sig::string_P;
        type.data.data.type = NULL;
        type.data.data.size = 0;
    } else {
        type.flags = 0;
        type.primitive = sig::pointer_P;
        type.data.data.type = internal->type_;
        type.data.data.size = 0;
    }

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_withName(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 1)
        throw CYJSError(context, "incorrect number of arguments to Type.withName");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));

    CYPool pool;
    const char *name(CYPoolCString(pool, context, arguments[0]));

    sig::Type type(*internal->type_);
    type.name = name;
    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 1)
        throw CYJSError(context, "incorrect number of arguments to type cast function");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));

    if (internal->type_->primitive == sig::function_P)
        return CYMakeFunctor(context, arguments[0], internal->type_->data.signature);

    sig::Type *type(internal->type_);
    ffi_type *ffi(internal->GetFFI());
    // XXX: alignment?
    uint8_t value[ffi->size];
    CYPool pool;
    CYPoolFFI(&pool, context, type, ffi, value, arguments[0]);
    return CYFromFFI(context, type, ffi, value);
} CYCatch(NULL) }

static JSObjectRef Type_callAsConstructor(JSContextRef context, JSObjectRef object, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 0)
        throw CYJSError(context, "incorrect number of arguments to Type allocator");
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));

    sig::Type *type(internal->type_);
    size_t length;

    if (type->primitive != sig::array_P)
        length = _not(size_t);
    else {
        length = type->data.data.size;
        type = type->data.data.type;
    }

    JSObjectRef pointer(CYMakePointer(context, NULL, length, type, NULL, NULL));
    Pointer *value(reinterpret_cast<Pointer *>(JSObjectGetPrivate(pointer)));
    value->value_ = value->pool_->malloc<void>(internal->GetFFI()->size);
    memset(value->value_, 0, internal->GetFFI()->size);
    return pointer;
} CYCatch(NULL) }

static JSObjectRef Functor_new(JSContextRef context, JSObjectRef object, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    if (count != 2)
        throw CYJSError(context, "incorrect number of arguments to Functor constructor");
    CYPool pool;
    const char *encoding(CYPoolCString(pool, context, arguments[1]));
    sig::Signature signature;
    sig::Parse(pool, &signature, encoding, &Structor_);
    return CYMakeFunctor(context, arguments[0], signature);
} CYCatch(NULL) }

static JSValueRef CString_callAsFunction_toPointer(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CString *internal(reinterpret_cast<CString *>(JSObjectGetPrivate(_this)));

    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = sig::char_P;
    type.data.data.type = NULL;
    type.data.data.size = 0;

    return CYMakePointer(context, internal->value_, _not(size_t), &type, NULL, NULL);
} CYCatch(NULL) }

static JSValueRef Functor_callAsFunction_$cya(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYPool pool;
    cy::Functor *internal(reinterpret_cast<cy::Functor *>(JSObjectGetPrivate(_this)));

    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = sig::function_P;
    sig::Copy(pool, type.data.signature, internal->signature_);

    return CYMakePointer(context, internal->value_, _not(size_t), &type, NULL, NULL);
} CYCatch(NULL) }

static JSValueRef Pointer_callAsFunction_toPointer(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    return _this;
} CYCatch(NULL) }

static JSValueRef CYValue_callAsFunction_valueOf(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYValue *internal(reinterpret_cast<CYValue *>(JSObjectGetPrivate(_this)));
    return CYCastJSValue(context, reinterpret_cast<uintptr_t>(internal->value_));
} CYCatch(NULL) }

static JSValueRef CYValue_callAsFunction_toJSON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) {
    return CYValue_callAsFunction_valueOf(context, object, _this, count, arguments, exception);
}

static JSValueRef CYValue_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    CYValue *internal(reinterpret_cast<CYValue *>(JSObjectGetPrivate(_this)));
    std::ostringstream str;
    Dl_info info;
    if (internal->value_ == NULL)
        str << "NULL";
    else if (dladdr(internal->value_, &info) == 0)
        str << internal->value_;
    else {
        str << info.dli_sname;
        off_t offset(static_cast<char *>(internal->value_) - static_cast<char *>(info.dli_saddr));
        if (offset != 0)
            str << "+0x" << std::hex << offset;
    }
    std::string value(str.str());
    return CYCastJSValue(context, CYJSString(CYUTF8String(value.c_str(), value.size())));
} CYCatch(NULL) }

static JSValueRef Pointer_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    std::set<void *> *objects(CYCastObjects(context, _this, count, arguments));

    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(_this)));
    if (internal->length_ != _not(size_t)) {
        JSObjectRef Array(CYGetCachedObject(context, CYJSString("Array_prototype")));
        JSObjectRef toCYON(CYCastJSObject(context, CYGetProperty(context, Array, toCYON_s)));
        return CYCallAsFunction(context, toCYON, _this, count, arguments);
    } else if (internal->type_->type_ == NULL) pointer: {
        CYLocalPool pool;
        std::ostringstream str;

        sig::Type type;
        type.name = NULL;
        type.flags = 0;

        type.primitive = sig::pointer_P;
        type.data.data.type = internal->type_->type_;
        type.data.data.size = 0;

        CYOptions options;
        CYOutput output(*str.rdbuf(), options);
        (new(pool) CYTypeExpression(Decode(pool, &type)))->Output(output, CYNoFlags);

        str << "(" << internal->value_ << ")";
        std::string value(str.str());
        return CYCastJSValue(context, CYJSString(CYUTF8String(value.c_str(), value.size())));
    } else try {
        JSValueRef value(CYGetProperty(context, _this, cyi_s));
        if (JSValueIsUndefined(context, value))
            goto pointer;
        CYPool pool;
        return CYCastJSValue(context, pool.strcat("&", CYPoolCCYON(pool, context, value, objects), NULL));
    } catch (const CYException &e) {
        goto pointer;
    }
} CYCatch(NULL) }

static JSValueRef CString_getProperty_length(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    CString *internal(reinterpret_cast<CString *>(JSObjectGetPrivate(object)));
    char *string(static_cast<char *>(internal->value_));
    return CYCastJSValue(context, strlen(string));
} CYCatch(NULL) }

static JSValueRef CString_getProperty_type(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    sig::Type type;
    type.name = NULL;
    type.flags = 0;

    type.primitive = sig::char_P;
    type.data.data.type = NULL;
    type.data.data.size = 0;

    return CYMakeType(context, &type);
} CYCatch(NULL) }

static JSValueRef Pointer_getProperty_type(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(object)));
    return CYMakeType(context, internal->type_->type_);
} CYCatch(NULL) }

static JSValueRef CString_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(_this)));
    const char *string(static_cast<const char *>(internal->value_));
    std::ostringstream str;
    str << "&";
    CYStringify(str, string, strlen(string), true);
    std::string value(str.str());
    return CYCastJSValue(context, CYJSString(CYUTF8String(value.c_str(), value.size())));
} CYCatch(NULL) }

static JSValueRef CString_callAsFunction_toString(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Pointer *internal(reinterpret_cast<Pointer *>(JSObjectGetPrivate(_this)));
    const char *string(static_cast<const char *>(internal->value_));
    return CYCastJSValue(context, string);
} CYCatch(NULL) }

static JSValueRef Functor_getProperty_type(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    cy::Functor *internal(reinterpret_cast<cy::Functor *>(JSObjectGetPrivate(object)));
    return CYMakeType(context, &internal->signature_);
} CYCatch(NULL) }

static JSValueRef Type_getProperty_alignment(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));
    return CYCastJSValue(context, internal->GetFFI()->alignment);
} CYCatch(NULL) }

static JSValueRef Type_getProperty_name(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));
    return CYCastJSValue(context, internal->type_->name);
} CYCatch(NULL) }

static JSValueRef Type_getProperty_size(JSContextRef context, JSObjectRef object, JSStringRef property, JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(object)));
    return CYCastJSValue(context, internal->GetFFI()->size);
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_toString(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));
    CYPool pool;
    const char *type(sig::Unparse(pool, internal->type_));
    return CYCastJSValue(context, CYJSString(type));
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_toCYON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    Type_privateData *internal(reinterpret_cast<Type_privateData *>(JSObjectGetPrivate(_this)));
    CYLocalPool pool;
    std::stringbuf out;
    CYOptions options;
    CYOutput output(out, options);
    (new(pool) CYTypeExpression(Decode(pool, internal->type_)))->Output(output, CYNoFlags);
    return CYCastJSValue(context, CYJSString(out.str().c_str()));
} CYCatch(NULL) }

static JSValueRef Type_callAsFunction_toJSON(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) {
    return Type_callAsFunction_toString(context, object, _this, count, arguments, exception);
}

static JSStaticFunction All_staticFunctions[2] = {
    {"cy$complete", &All_complete_callAsFunction, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

static JSStaticFunction CString_staticFunctions[6] = {
    {"toCYON", &CString_callAsFunction_toCYON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toJSON", &CYValue_callAsFunction_toJSON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toPointer", &CString_callAsFunction_toPointer, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toString", &CString_callAsFunction_toString, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"valueOf", &CString_callAsFunction_toString, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

static JSStaticValue CString_staticValues[3] = {
    {"length", &CString_getProperty_length, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"type", &CString_getProperty_type, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, NULL, 0}
};

static JSStaticFunction Pointer_staticFunctions[5] = {
    {"toCYON", &Pointer_callAsFunction_toCYON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toJSON", &CYValue_callAsFunction_toJSON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toPointer", &Pointer_callAsFunction_toPointer, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"valueOf", &CYValue_callAsFunction_valueOf, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

static JSStaticValue Pointer_staticValues[2] = {
    {"type", &Pointer_getProperty_type, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, NULL, 0}
};

static JSStaticFunction Struct_staticFunctions[2] = {
    {"$cya", &Struct_callAsFunction_$cya, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

static JSStaticFunction Functor_staticFunctions[5] = {
    {"$cya", &Functor_callAsFunction_$cya, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toCYON", &CYValue_callAsFunction_toCYON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toJSON", &CYValue_callAsFunction_toJSON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"valueOf", &CYValue_callAsFunction_valueOf, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

namespace cy {
    JSStaticFunction const * const Functor::StaticFunctions = Functor_staticFunctions;
}

static JSStaticValue Functor_staticValues[2] = {
    {"type", &Functor_getProperty_type, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, NULL, 0}
};

namespace cy {
    JSStaticValue const * const Functor::StaticValues = Functor_staticValues;
}

static JSStaticValue Type_staticValues[4] = {
    {"alignment", &Type_getProperty_alignment, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"name", &Type_getProperty_name, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"size", &Type_getProperty_size, NULL, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, NULL, 0}
};

static JSStaticFunction Type_staticFunctions[14] = {
    {"arrayOf", &Type_callAsFunction_arrayOf, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"blockWith", &Type_callAsFunction_blockWith, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"constant", &Type_callAsFunction_constant, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"functionWith", &Type_callAsFunction_functionWith, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"long", &Type_callAsFunction_long, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"pointerTo", &Type_callAsFunction_pointerTo, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"short", &Type_callAsFunction_short, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"signed", &Type_callAsFunction_signed, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"withName", &Type_callAsFunction_withName, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toCYON", &Type_callAsFunction_toCYON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toJSON", &Type_callAsFunction_toJSON, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"toString", &Type_callAsFunction_toString, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {"unsigned", &Type_callAsFunction_unsigned, kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete},
    {NULL, NULL, 0}
};

_visible void CYSetArgs(int argc, const char *argv[]) {
    JSContextRef context(CYGetJSContext());
    JSValueRef args[argc];
    for (int i(0); i != argc; ++i)
        args[i] = CYCastJSValue(context, argv[i]);

    JSObjectRef array(CYObjectMakeArray(context, argc, args));
    JSObjectRef System(CYGetCachedObject(context, CYJSString("System")));
    CYSetProperty(context, System, CYJSString("args"), array);
}

JSObjectRef CYGetGlobalObject(JSContextRef context) {
    return JSContextGetGlobalObject(context);
}

// XXX: this is neither exceptin safe nor even terribly sane
class ExecutionHandle {
  private:
    JSContextRef context_;
    std::vector<void *> handles_;

  public:
    ExecutionHandle(JSContextRef context) :
        context_(context)
    {
        handles_.resize(GetHooks().size());
        for (size_t i(0); i != GetHooks().size(); ++i) {
            CYHook *hook(GetHooks()[i]);
            if (hook->ExecuteStart != NULL)
                handles_[i] = (*hook->ExecuteStart)(context_);
            else
                handles_[i] = NULL;
        }
    }

    ~ExecutionHandle() {
        for (size_t i(GetHooks().size()); i != 0; --i) {
            CYHook *hook(GetHooks()[i-1]);
            if (hook->ExecuteEnd != NULL)
                (*hook->ExecuteEnd)(context_, handles_[i-1]);
        }
    }
};

static volatile bool cancel_;

static bool CYShouldTerminate(JSContextRef context, void *arg) {
    return cancel_;
}

_visible const char *CYExecute(JSContextRef context, CYPool &pool, CYUTF8String code) {
    ExecutionHandle handle(context);

    cancel_ = false;
    if (&JSContextGroupSetExecutionTimeLimit != NULL)
        JSContextGroupSetExecutionTimeLimit(JSContextGetGroup(context), 0.5, &CYShouldTerminate, NULL);

    try {
        JSValueRef result(_jsccall(JSEvaluateScript, context, CYJSString(code), NULL, NULL, 0));
        if (JSValueIsUndefined(context, result))
            return NULL;

        std::set<void *> objects;
        const char *json(_jsccall(CYPoolCCYON, pool, context, result, objects));
        CYSetProperty(context, CYGetGlobalObject(context), Result_, result);

        return json;
    } catch (const CYException &error) {
        return pool.strcat("throw ", error.PoolCString(pool), NULL);
    }
}

_visible void CYCancel() {
    cancel_ = true;
}

static const char *CYPoolLibraryPath(CYPool &pool);

static bool initialized_ = false;

void CYInitializeDynamic() {
    if (!initialized_)
        initialized_ = true;
    else return;

    CYPool pool;
    const char *db(pool.strcat(CYPoolLibraryPath(pool), "/libcycript.db", NULL));
    _sqlcall(sqlite3_open_v2(db, &database_, SQLITE_OPEN_READONLY, NULL));

    JSObjectMakeArray$ = reinterpret_cast<JSObjectRef (*)(JSContextRef, size_t, const JSValueRef[], JSValueRef *)>(dlsym(RTLD_DEFAULT, "JSObjectMakeArray"));
    JSSynchronousGarbageCollectForDebugging$ = reinterpret_cast<void (*)(JSContextRef)>(dlsym(RTLD_DEFAULT, "JSSynchronousGarbageCollectForDebugging"));

    JSClassDefinition definition;

    definition = kJSClassDefinitionEmpty;
    definition.className = "All";
    definition.staticFunctions = All_staticFunctions;
    definition.hasProperty = &All_hasProperty;
    definition.getProperty = &All_getProperty;
    definition.getPropertyNames = &All_getPropertyNames;
    All_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Context";
    definition.finalize = &CYFinalize;
    Context_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "CString";
    definition.staticFunctions = CString_staticFunctions;
    definition.staticValues = CString_staticValues;
    definition.getProperty = &CString_getProperty;
    definition.setProperty = &CString_setProperty;
    definition.finalize = &CYFinalize;
    CString_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Functor";
    definition.staticFunctions = cy::Functor::StaticFunctions;
    definition.staticValues = Functor_staticValues;
    definition.callAsFunction = &Functor_callAsFunction;
    definition.finalize = &CYFinalize;
    Functor_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Pointer";
    definition.staticFunctions = Pointer_staticFunctions;
    definition.staticValues = Pointer_staticValues;
    definition.callAsFunction = &Pointer_callAsFunction;
    definition.getProperty = &Pointer_getProperty;
    definition.setProperty = &Pointer_setProperty;
    definition.finalize = &CYFinalize;
    Pointer_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Struct";
    definition.staticFunctions = Struct_staticFunctions;
    definition.getProperty = &Struct_getProperty;
    definition.setProperty = &Struct_setProperty;
    definition.getPropertyNames = &Struct_getPropertyNames;
    definition.finalize = &CYFinalize;
    Struct_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Type";
    definition.staticValues = Type_staticValues;
    definition.staticFunctions = Type_staticFunctions;
    definition.callAsFunction = &Type_callAsFunction;
    definition.callAsConstructor = &Type_callAsConstructor;
    definition.finalize = &CYFinalize;
    Type_privateData::Class_ = JSClassCreate(&definition);

    definition = kJSClassDefinitionEmpty;
    definition.className = "Global";
    //definition.getProperty = &Global_getProperty;
    Global_ = JSClassCreate(&definition);

    Array_s = JSStringCreateWithUTF8CString("Array");
    cy_s = JSStringCreateWithUTF8CString("$cy");
    cyi_s = JSStringCreateWithUTF8CString("$cyi");
    length_s = JSStringCreateWithUTF8CString("length");
    message_s = JSStringCreateWithUTF8CString("message");
    name_s = JSStringCreateWithUTF8CString("name");
    pop_s = JSStringCreateWithUTF8CString("pop");
    prototype_s = JSStringCreateWithUTF8CString("prototype");
    push_s = JSStringCreateWithUTF8CString("push");
    splice_s = JSStringCreateWithUTF8CString("splice");
    toCYON_s = JSStringCreateWithUTF8CString("toCYON");
    toJSON_s = JSStringCreateWithUTF8CString("toJSON");
    toPointer_s = JSStringCreateWithUTF8CString("toPointer");
    toString_s = JSStringCreateWithUTF8CString("toString");
    weak_s = JSStringCreateWithUTF8CString("weak");

    Result_ = JSStringCreateWithUTF8CString("_");

    for (CYHook *hook : GetHooks())
        if (hook->Initialize != NULL)
            (*hook->Initialize)();
}

void CYThrow(JSContextRef context, JSValueRef value) {
    if (value != NULL)
        throw CYJSError(context, value);
}

const char *CYJSError::PoolCString(CYPool &pool) const {
    std::set<void *> objects;
    // XXX: this used to be CYPoolCString
    return CYPoolCCYON(pool, context_, value_, objects);
}

JSValueRef CYJSError::CastJSValue(JSContextRef context, const char *name) const {
    // XXX: what if the context is different? or the name? I dunno. ("epic" :/)
    return value_;
}

JSValueRef CYCastJSError(JSContextRef context, const char *name, const char *message) {
    JSObjectRef Error(CYGetCachedObject(context, CYJSString(name)));
    JSValueRef arguments[1] = {CYCastJSValue(context, message)};
    return _jsccall(JSObjectCallAsConstructor, context, Error, 1, arguments);
}

JSValueRef CYPoolError::CastJSValue(JSContextRef context, const char *name) const {
    return CYCastJSError(context, name, message_);
}

CYJSError::CYJSError(JSContextRef context, const char *format, ...) {
    _assert(context != NULL);

    CYPool pool;

    va_list args;
    va_start(args, format);
    // XXX: there might be a beter way to think about this
    const char *message(pool.vsprintf(64, format, args));
    va_end(args);

    value_ = CYCastJSError(context, "Error", message);
}

JSGlobalContextRef CYGetJSContext(JSContextRef context) {
    return reinterpret_cast<Context *>(JSObjectGetPrivate(CYCastJSObject(context, CYGetProperty(context, CYGetGlobalObject(context), cy_s))))->context_;
}

static const char *CYPoolLibraryPath(CYPool &pool) {
    Dl_info addr;
    _assert(dladdr(reinterpret_cast<void *>(&CYPoolLibraryPath), &addr) != 0);
    char *lib(pool.strdup(addr.dli_fname));

    char *slash(strrchr(lib, '/'));
    _assert(slash != NULL);
    *slash = '\0';

    slash = strrchr(lib, '/');
    if (slash != NULL && strcmp(slash, "/.libs") == 0)
        *slash = '\0';

    return lib;
}

static JSValueRef require_callAsFunction(JSContextRef context, JSObjectRef object, JSObjectRef _this, size_t count, const JSValueRef arguments[], JSValueRef *exception) { CYTry {
    _assert(count == 1);
    CYPool pool;

    const char *name(CYPoolCString(pool, context, arguments[0]));
    if (strchr(name, '/') == NULL && (
#ifdef __APPLE__
        dlopen(pool.strcat("/System/Library/Frameworks/", name, ".framework/", name, NULL), RTLD_LAZY | RTLD_GLOBAL) != NULL ||
        dlopen(pool.strcat("/System/Library/PrivateFrameworks/", name, ".framework/", name, NULL), RTLD_LAZY | RTLD_GLOBAL) != NULL ||
#endif
    false))
        return CYJSUndefined(context);

    JSObjectRef resolve(CYCastJSObject(context, CYGetProperty(context, object, CYJSString("resolve"))));
    CYJSString path(context, CYCallAsFunction(context, resolve, NULL, 1, arguments));

    CYJSString property("exports");

    JSObjectRef modules(CYGetCachedObject(context, CYJSString("modules")));
    JSValueRef cache(CYGetProperty(context, modules, path));

    JSValueRef result;
    if (!JSValueIsUndefined(context, cache)) {
        JSObjectRef module(CYCastJSObject(context, cache));
        result = CYGetProperty(context, module, property);
    } else {
        CYUTF8String code(CYPoolFileUTF8String(pool, CYPoolCString(pool, context, path)));
        _assert(code.data != NULL);

        size_t length(strlen(name));
        if (length >= 5 && strcmp(name + length - 5, ".json") == 0) {
            JSObjectRef JSON(CYGetCachedObject(context, CYJSString("JSON")));
            JSObjectRef parse(CYCastJSObject(context, CYGetProperty(context, JSON, CYJSString("parse"))));
            JSValueRef arguments[1] = { CYCastJSValue(context, CYJSString(code)) };
            result = CYCallAsFunction(context, parse, JSON, 1, arguments);
        } else {
            JSObjectRef module(JSObjectMake(context, NULL, NULL));
            CYSetProperty(context, modules, path, module);

            JSObjectRef exports(JSObjectMake(context, NULL, NULL));
            CYSetProperty(context, module, property, exports);

            std::stringstream wrap;
            wrap << "(function (exports, require, module, __filename) { " << code << "\n});";
            code = CYPoolCode(pool, *wrap.rdbuf());

            JSValueRef value(_jsccall(JSEvaluateScript, context, CYJSString(code), NULL, NULL, 0));
            JSObjectRef function(CYCastJSObject(context, value));

            JSValueRef arguments[4] = { exports, object, module, CYCastJSValue(context, path) };
            CYCallAsFunction(context, function, NULL, 4, arguments);
            result = CYGetProperty(context, module, property);
        }
    }

    return result;
} CYCatch(NULL) }

static bool CYRunScript(JSGlobalContextRef context, const char *path) {
    CYPool pool;
    CYUTF8String code(CYPoolFileUTF8String(pool, pool.strcat(CYPoolLibraryPath(pool), path, NULL)));
    if (code.data == NULL)
        return false;

    code = CYPoolCode(pool, code);
    _jsccall(JSEvaluateScript, context, CYJSString(code), NULL, NULL, 0);
    return true;
}

extern "C" void CYDestroyWeak(JSWeakObjectMapRef weak, void *data) {
}

extern "C" void CYSetupContext(JSGlobalContextRef context) {
    CYInitializeDynamic();

    JSObjectRef global(CYGetGlobalObject(context));

    JSObjectRef cy(JSObjectMake(context, Context_, new Context(context)));
    CYSetProperty(context, global, cy_s, cy, kJSPropertyAttributeDontEnum);

/* Cache Globals {{{ */
    JSObjectRef Array(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Array"))));
    CYSetProperty(context, cy, CYJSString("Array"), Array);

    JSObjectRef Array_prototype(CYCastJSObject(context, CYGetProperty(context, Array, prototype_s)));
    CYSetProperty(context, cy, CYJSString("Array_prototype"), Array_prototype);

    JSObjectRef Boolean(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Boolean"))));
    CYSetProperty(context, cy, CYJSString("Boolean"), Boolean);

    JSObjectRef Boolean_prototype(CYCastJSObject(context, CYGetProperty(context, Boolean, prototype_s)));
    CYSetProperty(context, cy, CYJSString("Boolean_prototype"), Boolean_prototype);

    JSObjectRef Error(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Error"))));
    CYSetProperty(context, cy, CYJSString("Error"), Error);

    JSObjectRef Function(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Function"))));
    CYSetProperty(context, cy, CYJSString("Function"), Function);

    JSObjectRef Function_prototype(CYCastJSObject(context, CYGetProperty(context, Function, prototype_s)));
    CYSetProperty(context, cy, CYJSString("Function_prototype"), Function_prototype);

    JSObjectRef JSON(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("JSON"))));
    CYSetProperty(context, cy, CYJSString("JSON"), JSON);

    JSObjectRef Number(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Number"))));
    CYSetProperty(context, cy, CYJSString("Number"), Number);

    JSObjectRef Number_prototype(CYCastJSObject(context, CYGetProperty(context, Number, prototype_s)));
    CYSetProperty(context, cy, CYJSString("Number_prototype"), Number_prototype);

    JSObjectRef Object(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("Object"))));
    CYSetProperty(context, cy, CYJSString("Object"), Object);

    JSObjectRef Object_prototype(CYCastJSObject(context, CYGetProperty(context, Object, prototype_s)));
    CYSetProperty(context, cy, CYJSString("Object_prototype"), Object_prototype);

    JSObjectRef String(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("String"))));
    CYSetProperty(context, cy, CYJSString("String"), String);

    JSObjectRef String_prototype(CYCastJSObject(context, CYGetProperty(context, String, prototype_s)));
    CYSetProperty(context, cy, CYJSString("String_prototype"), String_prototype);

    JSObjectRef SyntaxError(CYCastJSObject(context, CYGetProperty(context, global, CYJSString("SyntaxError"))));
    CYSetProperty(context, cy, CYJSString("SyntaxError"), SyntaxError);
/* }}} */

    CYSetProperty(context, Array_prototype, toCYON_s, &Array_callAsFunction_toCYON, kJSPropertyAttributeDontEnum);
    CYSetProperty(context, String_prototype, toCYON_s, &String_callAsFunction_toCYON, kJSPropertyAttributeDontEnum);

    JSObjectRef cycript(JSObjectMake(context, NULL, NULL));
    CYSetProperty(context, global, CYJSString("Cycript"), cycript);
    CYSetProperty(context, cycript, CYJSString("compile"), &Cycript_compile_callAsFunction);
    CYSetProperty(context, cycript, CYJSString("gc"), &Cycript_gc_callAsFunction);

    JSObjectRef CString(JSObjectMakeConstructor(context, CString_, &CString_new));
    CYSetPrototype(context, CYCastJSObject(context, CYGetProperty(context, CString, prototype_s)), String_prototype);
    CYSetProperty(context, cycript, CYJSString("CString"), CString);

    JSObjectRef Functor(JSObjectMakeConstructor(context, Functor_, &Functor_new));
    CYSetPrototype(context, CYCastJSObject(context, CYGetProperty(context, Functor, prototype_s)), Function_prototype);
    CYSetProperty(context, cycript, CYJSString("Functor"), Functor);

    CYSetProperty(context, cycript, CYJSString("Pointer"), JSObjectMakeConstructor(context, Pointer_, &Pointer_new));
    CYSetProperty(context, cycript, CYJSString("Type"), JSObjectMakeConstructor(context, Type_privateData::Class_, &Type_new));

    JSObjectRef modules(JSObjectMake(context, NULL, NULL));
    CYSetProperty(context, cy, CYJSString("modules"), modules);

    JSObjectRef all(JSObjectMake(context, All_, NULL));
    CYSetProperty(context, cycript, CYJSString("all"), all);

    JSObjectRef cache(JSObjectMake(context, NULL, NULL));
    CYSetProperty(context, cy, CYJSString("cache"), cache);
    CYSetPrototype(context, cache, all);

    JSObjectRef alls(_jsccall(JSObjectCallAsConstructor, context, Array, 0, NULL));
    CYSetProperty(context, cycript, CYJSString("alls"), alls);

    if (true) {
        JSObjectRef last(NULL), curr(global);

        goto next; for (JSValueRef next;;) {
            if (JSValueIsNull(context, next))
                break;
            last = curr;
            curr = CYCastJSObject(context, next);
          next:
            next = JSObjectGetPrototype(context, curr);
        }

        CYSetPrototype(context, last, cache);
    }

    JSObjectRef System(JSObjectMake(context, NULL, NULL));
    CYSetProperty(context, cy, CYJSString("System"), System);

    CYSetProperty(context, global, CYJSString("require"), &require_callAsFunction, kJSPropertyAttributeDontEnum);

    CYSetProperty(context, global, CYJSString("system"), System);
    CYSetProperty(context, System, CYJSString("args"), CYJSNull(context));
    CYSetProperty(context, System, CYJSString("print"), &System_print);

    CYSetProperty(context, global, CYJSString("global"), global);

#ifdef __APPLE__
    if (&JSWeakObjectMapCreate != NULL) {
        JSWeakObjectMapRef weak(JSWeakObjectMapCreate(context, NULL, &CYDestroyWeak));
        CYSetProperty(context, cy, weak_s, CYCastJSValue(context, reinterpret_cast<uintptr_t>(weak)));
    }
#endif

    CYSetProperty(context, cache, CYJSString("dlerror"), CYMakeFunctor(context, "dlerror", "*"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("RTLD_DEFAULT"), CYCastJSValue(context, reinterpret_cast<intptr_t>(RTLD_DEFAULT)), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("dlsym"), CYMakeFunctor(context, "dlsym", "^v^v*"), kJSPropertyAttributeDontEnum);

    CYSetProperty(context, cache, CYJSString("NULL"), CYJSNull(context), kJSPropertyAttributeDontEnum);

    CYSetProperty(context, cache, CYJSString("bool"), CYMakeType(context, "B"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("char"), CYMakeType(context, "c"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("short"), CYMakeType(context, "s"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("int"), CYMakeType(context, "i"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("long"), CYMakeType(context, "l"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("float"), CYMakeType(context, "f"), kJSPropertyAttributeDontEnum);
    CYSetProperty(context, cache, CYJSString("double"), CYMakeType(context, "d"), kJSPropertyAttributeDontEnum);

    for (CYHook *hook : GetHooks())
        if (hook->SetupContext != NULL)
            (*hook->SetupContext)(context);

    CYArrayPush(context, alls, cycript);

    CYRunScript(context, "/libcycript.cy");
}

static JSGlobalContextRef context_;

_visible JSGlobalContextRef CYGetJSContext() {
    CYInitializeDynamic();

    if (context_ == NULL) {
        context_ = JSGlobalContextCreate(Global_);
        CYSetupContext(context_);
    }

    return context_;
}

_visible void CYDestroyContext() {
    if (context_ == NULL)
        return;
    JSGlobalContextRelease(context_);
    context_ = NULL;
}
