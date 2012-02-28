//
// Copyright (C) 2011 Andrey Sibiryov <me@kobology.ru>
//
// Licensed under the BSD 2-Clause License (the "License");
// you may not use this file except in compliance with the License.
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <sstream>
#include <boost/filesystem/fstream.hpp>

#include "python.hpp"

using namespace cocaine::core;
using namespace cocaine::engine;

cocaine::object_t* python_t::create(context_t& ctx) {
    return new python_t(ctx);
}

python_t::python_t(context_t& ctx):
    plugin_t(ctx, "python"),
    m_python_module(NULL)
{ }

void python_t::initialize(const app_t& app) {
    boost::filesystem::path source(app.args["source"].asString());
    
    if(source.empty()) {
        throw unrecoverable_error_t("no code location has been specified");
    }

    boost::filesystem::ifstream input(source);
    
    if(!input) {
        throw unrecoverable_error_t("unable to open " + source.string());
    }

    char path_object_name[] = "path";

    // Acquire the interpreter state
    thread_state_t state;
    
    // NOTE: Prepend the current application cache location to the sys.path,
    // so that it could import different stuff from there
    PyObject* syspath = PySys_GetObject(path_object_name);

    if(PyList_Check(syspath)) {
        std::stringstream stream;
        stream << input.rdbuf();

        // XXX: Does it steal the reference or not?
#if BOOST_FILESYSTEM_VERSION == 3
        PyList_Insert(syspath, 0, 
            PyString_FromString(source.parent_path().string().c_str()));
#else
        PyList_Insert(syspath, 0, 
            PyString_FromString(source.branch_path().string().c_str()));
#endif

        compile(source.string(), stream.str());
    } else {
        throw unrecoverable_error_t("'sys.path' is not a list object");
    }
}

void python_t::invoke(invocation_site_t& site) {
    thread_state_t state;
    
    if(!m_python_module) {
        throw unrecoverable_error_t("python module is not initialized");
    }

    python_object_t object(
        PyObject_GetAttrString(
            m_python_module, 
            site.method.c_str()
        )
    );
    
    if(PyErr_Occurred()) {
        throw unrecoverable_error_t(exception());
    }

    if(PyType_Check(object)) {
        if(PyType_Ready(reinterpret_cast<PyTypeObject*>(*object)) != 0) {
            throw unrecoverable_error_t(exception());
        }
    }

    if(!PyCallable_Check(object)) {
        throw unrecoverable_error_t("'" + site.method + "' is not callable");
    }

    python_object_t args(NULL);
#if PY_VERSION_HEX >= 0x02070000
    boost::shared_ptr<Py_buffer> buffer;
#endif

    if(site.request && site.request_size) {
#if PY_VERSION_HEX >= 0x02070000
        buffer.reset(static_cast<Py_buffer*>(malloc(sizeof(Py_buffer))), free);

        buffer->buf = const_cast<void*>(site.request);
        buffer->len = site.request_size;
        buffer->readonly = true;
        buffer->format = NULL;
        buffer->ndim = 0;
        buffer->shape = NULL;
        buffer->strides = NULL;
        buffer->suboffsets = NULL;

        python_object_t view(PyMemoryView_FromBuffer(buffer.get()));
#else
        python_object_t view(PyBuffer_FromMemory(
            const_cast<void*>(site.request), 
            site.request_size));
#endif

        args = PyTuple_Pack(1, *view);
    } else {
        args = PyTuple_New(0);
    }

    python_object_t result(PyObject_Call(object, args, NULL));

    if(PyErr_Occurred()) {
        throw recoverable_error_t(exception());
    } else if(result.valid()) {
        respond(site, result);
    }
}

void python_t::respond(invocation_site_t& site, python_object_t& result) {
    if(PyString_Check(result)) {
        throw recoverable_error_t("the result must be an iterable");
    }

    python_object_t iterator(PyObject_GetIter(result));

    if(iterator.valid()) {
        python_object_t item(NULL);

        while(true) {
            item = PyIter_Next(iterator);

            if(PyErr_Occurred()) {
                throw recoverable_error_t(exception());
            } else if(!item.valid()) {
                break;
            }
        
#if PY_VERSION_HEX > 0x02060000
            if(PyObject_CheckBuffer(item)) {
                boost::shared_ptr<Py_buffer> buffer(
                    static_cast<Py_buffer*>(malloc(sizeof(Py_buffer))),
                    free);

                if(PyObject_GetBuffer(item, buffer.get(), PyBUF_SIMPLE) == 0) {
                    Py_BEGIN_ALLOW_THREADS
                        site.push(buffer->buf, buffer->len);
                    Py_END_ALLOW_THREADS
                    
                    PyBuffer_Release(buffer.get());
                } else {
                    throw recoverable_error_t("unable to serialize the result");
                }
            }
#else
            if(PyString_Check(item)) {
                callback(PyString_AsString(item), PyString_Size(item));
            } else {
                throw recoverable_error_t("unable to serialize the result");
            }
#endif
        }
    } else {
        throw recoverable_error_t(exception());
    }
}

std::string python_t::exception() {
    python_object_t type(NULL), object(NULL), traceback(NULL);
    
    PyErr_Fetch(&type, &object, &traceback);
    python_object_t message(PyObject_Str(object));
    
    return PyString_AsString(message);
}

void python_t::compile(const std::string& path, const std::string& code) {
    python_object_t bytecode(Py_CompileString(
        code.c_str(),
        path.c_str(),
        Py_file_input));

    if(PyErr_Occurred()) {
        throw unrecoverable_error_t(exception());
    }

    m_python_module = PyImport_ExecCodeModule(
        const_cast<char*>(unique_id_t().id().c_str()),
        bytecode);
    
    if(PyErr_Occurred()) {
        throw unrecoverable_error_t(exception());
    }
}

static const module_info_t module_info[] = {
    { "python", &python_t::create },
    { "python+raw", &python_t::create },
    { NULL, NULL }
};

PyThreadState* g_state = NULL;

void save() {
    g_state = PyEval_SaveThread();
}

void restore() {
    PyEval_RestoreThread(g_state);
}

extern "C" {
    const module_info_t* initialize() {
        // Initialize the Python subsystem
        Py_InitializeEx(0);

        // Initialize the GIL
        PyEval_InitThreads();
        save();

        // NOTE: In case of a fork, restore the main thread state and acquire the GIL,
        // call the python post-fork handler and save the main thread again, releasing the GIL.
        pthread_atfork(NULL, NULL, restore);
        pthread_atfork(NULL, NULL, PyOS_AfterFork);
        pthread_atfork(NULL, NULL, save);

        return module_info;
    }

    __attribute__((destructor)) void finalize() {
        restore();
        Py_Finalize();
    }
}
