/*
 * Copyright 2006 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdio.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "wine/debug.h"

#include "mshtml_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

#define WM_PROCESSTASK 0x8008
#define WM_CREATEDOC   0x8018
#define TIMER_ID 0x3000

typedef struct {
    HTMLInnerWindow *window;
    DWORD id;
    DWORD time;
    DWORD interval;
    enum timer_type type;
    IDispatch *disp;

    struct list entry;
} task_timer_t;

static void default_task_destr(task_t *task)
{
    heap_free(task);
}

HRESULT push_task(task_t *task, task_proc_t proc, task_proc_t destr, LONG magic)
{
    thread_data_t *thread_data;

    thread_data = get_thread_data(TRUE);
    if(!thread_data) {
        if(destr)
            destr(task);
        else
            heap_free(task);
        return E_OUTOFMEMORY;
    }

    task->target_magic = magic;
    task->proc = proc;
    task->destr = destr ? destr : default_task_destr;

    list_add_tail(&thread_data->task_list, &task->entry);

    PostMessageW(thread_data->thread_hwnd, WM_PROCESSTASK, 0, 0);
    return S_OK;
}

static task_t *pop_task(void)
{
    thread_data_t *thread_data;
    task_t *task;

    thread_data = get_thread_data(FALSE);
    if(!thread_data)
        return NULL;

    if(list_empty(&thread_data->task_list))
        return NULL;

    task = LIST_ENTRY(thread_data->task_list.next, task_t, entry);
    list_remove(&task->entry);
    return task;
}

static void release_task_timer(HWND thread_hwnd, task_timer_t *timer)
{
    list_remove(&timer->entry);

    IDispatch_Release(timer->disp);

    heap_free(timer);
}

void remove_target_tasks(LONG target)
{
    thread_data_t *thread_data = get_thread_data(FALSE);
    struct list *liter, *ltmp;
    task_timer_t *timer;
    task_t *task;

    if(!thread_data)
        return;

    LIST_FOR_EACH_SAFE(liter, ltmp, &thread_data->timer_list) {
        timer = LIST_ENTRY(liter, task_timer_t, entry);
        if(timer->window->task_magic == target)
            release_task_timer(thread_data->thread_hwnd, timer);
    }

    if(!list_empty(&thread_data->timer_list)) {
        DWORD tc = GetTickCount();

        timer = LIST_ENTRY(list_head(&thread_data->timer_list), task_timer_t, entry);
        SetTimer(thread_data->thread_hwnd, TIMER_ID, max( (int)(timer->time - tc), 0 ), NULL);
    }

    LIST_FOR_EACH_SAFE(liter, ltmp, &thread_data->task_list) {
        task = LIST_ENTRY(liter, task_t, entry);
        if(task->target_magic == target) {
            list_remove(&task->entry);
            task->destr(task);
        }
    }
}

LONG get_task_target_magic(void)
{
    static LONG magic = 0x10000000;
    return InterlockedIncrement(&magic);
}

static BOOL queue_timer(thread_data_t *thread_data, task_timer_t *timer)
{
    task_timer_t *iter;

    list_remove(&timer->entry);

    if(list_empty(&thread_data->timer_list)
       || LIST_ENTRY(list_head(&thread_data->timer_list), task_timer_t, entry)->time > timer->time) {

        list_add_head(&thread_data->timer_list, &timer->entry);
        return TRUE;
    }

    LIST_FOR_EACH_ENTRY(iter, &thread_data->timer_list, task_timer_t, entry) {
        if(iter->time > timer->time) {
            list_add_tail(&iter->entry, &timer->entry);
            return FALSE;
        }
    }

    list_add_tail(&thread_data->timer_list, &timer->entry);
    return FALSE;
}

HRESULT set_task_timer(HTMLInnerWindow *window, LONG msec, enum timer_type timer_type, IDispatch *disp, LONG *id)
{
    thread_data_t *thread_data;
    task_timer_t *timer;
    DWORD tc = GetTickCount();

    static DWORD id_cnt = 0x20000000;

    thread_data = get_thread_data(TRUE);
    if(!thread_data)
        return E_OUTOFMEMORY;

    timer = heap_alloc(sizeof(task_timer_t));
    if(!timer)
        return E_OUTOFMEMORY;

    if(msec < 1)
        msec = 1;

    timer->id = id_cnt++;
    timer->window = window;
    timer->time = tc + msec;
    timer->interval = timer_type == TIMER_INTERVAL ? msec : 0;
    timer->type = timer_type;
    list_init(&timer->entry);

    IDispatch_AddRef(disp);
    timer->disp = disp;

    if(queue_timer(thread_data, timer))
        SetTimer(thread_data->thread_hwnd, TIMER_ID, msec, NULL);

    *id = timer->id;
    return S_OK;
}

HRESULT clear_task_timer(HTMLInnerWindow *window, DWORD id)
{
    thread_data_t *thread_data = get_thread_data(FALSE);
    task_timer_t *iter;

    if(!thread_data)
        return S_OK;

    LIST_FOR_EACH_ENTRY(iter, &thread_data->timer_list, task_timer_t, entry) {
        if(iter->id == id && iter->window == window) {
            if(iter->type != TIMER_ANIMATION_FRAME)
                release_task_timer(thread_data->thread_hwnd, iter);
            return S_OK;
        }
    }

    WARN("timet not found\n");
    return S_OK;
}

HRESULT clear_animation_timer(HTMLInnerWindow *window, DWORD id)
{
    thread_data_t *thread_data = get_thread_data(FALSE);
    task_timer_t *iter;

    if(!thread_data)
        return S_OK;

    LIST_FOR_EACH_ENTRY(iter, &thread_data->timer_list, task_timer_t, entry) {
        if(iter->id == id && iter->window == window) {
            if(iter->type == TIMER_ANIMATION_FRAME)
                release_task_timer(thread_data->thread_hwnd, iter);
            return S_OK;
        }
    }

    WARN("timer not found\n");
    return S_OK;
}

static const char *debugstr_timer_type(enum timer_type type)
{
    switch(type) {
    case TIMER_TIMEOUT:  return "timeout";
    case TIMER_INTERVAL: return "interval";
    case TIMER_ANIMATION_FRAME: return "animation-frame";
    DEFAULT_UNREACHABLE;
    }
}

static void call_timer_disp(IDispatch *disp, enum timer_type timer_type)
{
    DISPPARAMS dp = {NULL, NULL, 0, 0};
    VARIANT timestamp;
    EXCEPINFO ei;
    VARIANT res;
    HRESULT hres;

    V_VT(&res) = VT_EMPTY;
    memset(&ei, 0, sizeof(ei));

    if(timer_type == TIMER_ANIMATION_FRAME) {
        dp.cArgs = 1;
        dp.rgvarg = &timestamp;
        V_VT(&timestamp) = VT_R8;
        V_R8(&timestamp) = get_time_stamp();
    }

    TRACE("%p %s >>>\n", disp, debugstr_timer_type(timer_type));
    hres = IDispatch_Invoke(disp, DISPID_VALUE, &IID_NULL, 0, DISPATCH_METHOD, &dp, &res, &ei, NULL);
    if(hres == S_OK)
        TRACE("%p %s <<<\n", disp, debugstr_timer_type(timer_type));
    else
        WARN("%p %s <<< %08lx\n", disp, debugstr_timer_type(timer_type), hres);

    VariantClear(&res);
}

static LRESULT process_timer(void)
{
    thread_data_t *thread_data;
    enum timer_type timer_type;
    IDispatch *disp;
    DWORD tc;
    task_timer_t *timer=NULL, *last_timer;

    TRACE("\n");

    thread_data = get_thread_data(FALSE);
    assert(thread_data != NULL);

    if(list_empty(&thread_data->timer_list)) {
        KillTimer(thread_data->thread_hwnd, TIMER_ID);
        return 0;
    }

    last_timer = LIST_ENTRY(list_tail(&thread_data->timer_list), task_timer_t, entry);
    do {
        tc = GetTickCount();
        if(timer == last_timer) {
            timer = LIST_ENTRY(list_head(&thread_data->timer_list), task_timer_t, entry);
            SetTimer(thread_data->thread_hwnd, TIMER_ID, timer->time>tc ? timer->time-tc : 0, NULL);
            return 0;
        }

        timer = LIST_ENTRY(list_head(&thread_data->timer_list), task_timer_t, entry);

        if(timer->time > tc) {
            SetTimer(thread_data->thread_hwnd, TIMER_ID, timer->time-tc, NULL);
            return 0;
        }

        disp = timer->disp;
        IDispatch_AddRef(disp);
        timer_type = timer->type;

        if(timer->interval) {
            timer->time += timer->interval;
            queue_timer(thread_data, timer);
        }else {
            release_task_timer(thread_data->thread_hwnd, timer);
        }

        call_timer_disp(disp, timer_type);

        IDispatch_Release(disp);
    }while(!list_empty(&thread_data->timer_list));

    KillTimer(thread_data->thread_hwnd, TIMER_ID);
    return 0;
}

typedef struct {
    IUnknown *unk;
    IID iid;
    IStream *stream;
    HRESULT hres;
} create_doc_params_t;

static LRESULT WINAPI hidden_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg) {
    case WM_PROCESSTASK:
        while(1) {
            task_t *task = pop_task();
            if(!task)
                break;

            task->proc(task);
            task->destr(task);
        }

        return 0;
    case WM_TIMER:
        return process_timer();
    case WM_CREATEDOC: {
        create_doc_params_t *params = (create_doc_params_t*)lParam;
        IUnknown *unk;

        TRACE("WM_CREATEDOC %p\n", params);

        params->hres = HTMLDocument_Create(NULL, &params->iid, (void**)&unk);
        if(FAILED(params->hres))
            return 0;

        params->hres = CoMarshalInterface(params->stream, &params->iid, unk, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
        IUnknown_Release(unk);
        return 0;
    }
    }

    if(msg > WM_USER)
        FIXME("(%p %d %Ix %Ix)\n", hwnd, msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND create_thread_hwnd(void)
{
    static ATOM hidden_wnd_class = 0;

    if(!hidden_wnd_class) {
        WNDCLASSEXW wndclass = {
            sizeof(WNDCLASSEXW), 0,
            hidden_proc,
            0, 0, hInst, NULL, NULL, NULL, NULL,
            L"Internet Explorer_Hidden",
            NULL
        };

        hidden_wnd_class = RegisterClassExW(&wndclass);
    }

    return CreateWindowExW(0, L"Internet Explorer_Hidden", NULL, WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hInst, NULL);
}

HWND get_thread_hwnd(void)
{
    thread_data_t *thread_data;

    thread_data = get_thread_data(TRUE);
    if(!thread_data)
        return NULL;

    if(!thread_data->thread_hwnd)
        thread_data->thread_hwnd = create_thread_hwnd();

    return thread_data->thread_hwnd;
}

HRESULT create_marshaled_doc(HWND main_thread_hwnd, REFIID riid, void **ppv)
{
    create_doc_params_t params = {NULL, *riid, NULL, E_FAIL};
    LARGE_INTEGER zero;
    BOOL res;
    HRESULT hres;

    hres = CreateStreamOnHGlobal(NULL, TRUE, &params.stream);
    if(FAILED(hres))
        return hres;

    res = SendMessageW(main_thread_hwnd, WM_CREATEDOC, 0, (LPARAM)&params);
    TRACE("SendMessage ret %x\n", res);
    if(FAILED(params.hres)) {
        WARN("EM_CREATEDOC failed: %08x\n", params.hres);
        IStream_Release(params.stream);
        return hres;
    }

    zero.QuadPart = 0;
    hres = IStream_Seek(params.stream, zero, STREAM_SEEK_SET, NULL);
    if(SUCCEEDED(hres))
        hres = CoUnmarshalInterface(params.stream, riid, ppv);
    IStream_Release(params.stream);
    if(FAILED(hres))
        WARN("CoUnmarshalInterface failed: %08x\n", hres);
    return hres;
}

thread_data_t *get_thread_data(BOOL create)
{
    thread_data_t *thread_data;

    if(mshtml_tls == TLS_OUT_OF_INDEXES) {
        DWORD tls;

        if(!create)
            return NULL;

        tls = TlsAlloc();
        if(tls == TLS_OUT_OF_INDEXES)
            return NULL;

        tls = InterlockedCompareExchange((LONG*)&mshtml_tls, tls, TLS_OUT_OF_INDEXES);
        if(tls != mshtml_tls)
            TlsFree(tls);
    }

    thread_data = TlsGetValue(mshtml_tls);
    if(!thread_data && create) {
        thread_data = heap_alloc_zero(sizeof(thread_data_t));
        if(!thread_data)
            return NULL;

        TlsSetValue(mshtml_tls, thread_data);
        list_init(&thread_data->task_list);
        list_init(&thread_data->timer_list);
    }

    return thread_data;
}

ULONGLONG get_time_stamp(void)
{
    FILETIME time;

    /* 1601 to 1970 is 369 years plus 89 leap days */
    const ULONGLONG time_epoch = (ULONGLONG)(369 * 365 + 89) * 86400 * 1000;

    GetSystemTimeAsFileTime(&time);
    return (((ULONGLONG)time.dwHighDateTime << 32) + time.dwLowDateTime) / 10000 - time_epoch;
}
