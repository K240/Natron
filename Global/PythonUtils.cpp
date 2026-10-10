/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#undef Py_LIMITED_API  // PyConfig is part of the full embedding API.
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PythonUtils.h"

#include <cstdlib>
#include <vector>
#include <stdexcept>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma warning (disable : 4996)
#else
#include <dirent.h>
#endif

#include "../Global/FStreamsSupport.h"
#include "../Global/ProcInfo.h"
#include "../Global/StrUtils.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

static bool fileExists(const std::string& path)
{
    FStreamsSupport::ifstream ifile;
    FStreamsSupport::open(&ifile, path);
    return ifile.good();
}

static bool dirExists(const std::string& path)
{
    // https://stackoverflow.com/q/18100097
    struct stat info;

    if(stat(path.c_str(), &info ) != 0) {
        return false;
    } else if(info.st_mode & S_IFDIR) {
        return true;
    }
    return false;
}

void setupPythonEnv(const std::string& binPath)
{
    //Disable user sites as they could conflict with Natron bundled packages.
    //If this is set, Python wonâ€™t add the user site-packages directory to sys.path.
    //See https://www.python.org/dev/peps/pep-0370/
    ProcInfo::putenv_wrapper("PYTHONNOUSERSITE", "1");

    //
    // set up paths, clear those that don't exist or are not valid
    //
#ifdef __NATRON_WIN32__
    std::string pythonHome = binPath + "\\..";
    std::string pyPathZip = pythonHome + "\\python" NATRON_PY_VERSION_STRING_NO_DOT ".zip";
    std::string pyPath = pythonHome + "\\Lib";
    std::string pyPathDynLoad = pythonHome + "\\DLLs";
    std::string pyPathSitePackages = pyPath + "\\site-packages";
    std::string pluginPath = binPath + "\\..\\Plugins";
#else
#  if defined(__NATRON_LINUX__)
    std::string pythonHome = binPath + "/..";
#  elif defined(__NATRON_OSX__)
    std::string pythonHome = binPath+ "/../Frameworks/Python.framework/Versions/" NATRON_PY_VERSION_STRING;
#  else
#    error "unsupported platform"
#  endif
    std::string pyPathZip = pythonHome + "/lib/python" NATRON_PY_VERSION_STRING_NO_DOT ".zip";
    std::string pyPath = pythonHome + "/lib/python" NATRON_PY_VERSION_STRING;
    std::string pyPathDynLoad = pyPath + "/lib-dynload";
    std::string pyPathSitePackages = pyPath + "/site-packages";
    std::string pluginPath = binPath + "/../Plugins";
#endif
    if ( !fileExists( StrUtils::fromNativeSeparators(pyPathZip) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathZip.c_str() );
#     endif
        pyPathZip.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPath) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPath.c_str() );
#     endif
        pyPath.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPathDynLoad) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathDynLoad.c_str() );
#     endif
        pyPathDynLoad.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pyPathSitePackages) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pyPathSitePackages.c_str() );
#     endif
        pyPathSitePackages.clear();
    }
    if ( !dirExists( StrUtils::fromNativeSeparators(pluginPath) ) ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "\"%s\" does not exist, not added to PYTHONPATH\n", pluginPath.c_str() );
#     endif
        pluginPath.clear();
    }
    // PYTHONHOME is really useful if there's a python inside it
    if ( pyPathZip.empty() && pyPath.empty() ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "dir \"%s\" does not exist or does not contain lib/python*, not setting PYTHONHOME\n", pythonHome.c_str() );
#     endif
        pythonHome.clear();
    }

    if (!pythonHome.empty()) {
        ProcInfo::putenv_wrapper("PYTHONHOME", pythonHome.c_str());
    }

    std::string pythonPath = ProcInfo::getenv_wrapper("PYTHONPATH");
    //Add the Python distribution of Natron to the Python path

    std::vector<std::string> toPrepend;
    if ( !pyPathZip.empty() ) {
        toPrepend.push_back(pyPathZip);
    }
    if ( !pyPath.empty() ) {
        toPrepend.push_back(pyPath);
    }
    if ( !pyPathDynLoad.empty() ) {
        toPrepend.push_back(pyPathDynLoad);
    }
    if ( !pyPathSitePackages.empty() ) {
        toPrepend.push_back(pyPathSitePackages);
    }
    if ( !pluginPath.empty() ) {
        toPrepend.push_back(pluginPath);
    }

    if ( toPrepend.empty() ) {
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf("PYTHONPATH not modified\n");
#     endif
    } else {
#     ifdef __NATRON_WIN32__
        const char pathSep = ';';
#     else
        const char pathSep = ':';
#     endif
        std::string toPrependStr = StrUtils::join(toPrepend, pathSep);
        if (pythonPath.empty()) {
            pythonPath = toPrependStr;
        } else {
            pythonPath = toPrependStr + pathSep + pythonPath;
        }
        // Keep the interpreter's standard library search paths.
        ProcInfo::putenv_wrapper( "PYTHONPATH", pythonPath.c_str() );
#     if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
        printf( "PYTHONPATH set to %s\n", pythonPath.c_str() );
#     endif
    }

} // setupPythonEnv

PyObject* initializePython3(const std::vector<wchar_t*>& commandLineArgsWide)
{
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0; // Natron handles its own command-line options.
    config.user_site_directory = 0;
    config.install_signal_handlers = 0;

    PyStatus status = PyConfig_SetString(&config, &config.stdio_encoding, L"utf-8");
    if (!PyStatus_Exception(status) && !commandLineArgsWide.empty()) {
        status = PyConfig_SetString(&config, &config.program_name, commandLineArgsWide.front());
        if (!PyStatus_Exception(status)) {
            status = PyConfig_SetArgv(&config, commandLineArgsWide.size(),
                const_cast<wchar_t**>(commandLineArgsWide.data()));
        }
    }
    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }
    const std::string error = PyStatus_Exception(status) ?
        (status.err_msg ? status.err_msg : "Python initialization failed") : "";
    PyConfig_Clear(&config);
    if (!error.empty()) {
        throw std::runtime_error(error);
    }

    PyObject* mainModule = PyImport_ImportModule("__main__");
    if (!mainModule) {
        PyErr_Print();
        throw std::runtime_error("Could not import Python's __main__ module");
    }
#if defined(NATRON_CONFIG_SNAPSHOT) || defined(DEBUG)
    PySys_FormatStdout("Python %s\n", Py_GetVersion());
    PySys_FormatStdout("  sys.prefix = %A\n", PySys_GetObject("prefix"));
    PySys_FormatStdout("  sys.path = %A\n", PySys_GetObject("path"));
#endif
    // Release the interpreter's initial GIL; subsequent API calls use PythonGILLocker.
    PyEval_SaveThread();
    return mainModule;
} // initializePython


std::string
PyStringToStdString(PyObject* py_val)
{
    ///Must be locked
    assert( PyThreadState_Get() );
    std::string val;
    PyObject* s = nullptr;
    if (!py_val) {
        val = "(null)";
    } else if( PyUnicode_Check(py_val) ) {
        s = PyUnicode_AsUTF8String(py_val);
    } else if( PyBytes_Check(py_val) ) {
        s = PyObject_Bytes(py_val);
    } else {
        // Not a string => Error, warning ...
        val = "(not a string)";
    }

    // If succesfully converted to bytes, then convert to C++ string
    if (s) {
        val = std::string( PyBytes_AS_STRING(s) );
        Py_XDECREF(s);
    }

    return val;
}

NATRON_PYTHON_NAMESPACE_EXIT

NATRON_NAMESPACE_EXIT
