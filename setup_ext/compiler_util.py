"""
Distutils Compiler Utilities

This module provides utilities for working with distutils compiler types,
including information about all available compilers and platform mappings.
"""

import os
import sys
import re
from typing import Dict, List, Tuple, Optional, Any, Union
from distutils import ccompiler
from distutils.util import get_platform

# All known distutils compiler types
COMPILER_TYPES = [
    'unix',  # Standard UNIX-style compiler (GCC/Clang)
    'msvc',  # Microsoft Visual C++
    'cygwin',  # Cygwin port of GNU C Compiler for Win32
    'mingw32',  # MinGW32 port of GNU C Compiler for Win32
    'bcpp',  # Borland C++ Compiler (may not be available)
    'zos',  # IBM XL C/C++ Compilers
]

# Compiler descriptions
COMPILER_DESCRIPTIONS = {
    'unix': 'Standard UNIX-style compiler (GCC/Clang)',
    'msvc': 'Microsoft Visual C++',
    'cygwin': 'Cygwin port of GNU C Compiler for Win32',
    'mingw32': 'MinGW32 port of GNU C Compiler for Win32',
    'bcpp': 'Borland C++ Compiler',
    'zos': 'IBM XL C/C++ Compilers',
}

# Platform to default compiler mapping (from distutils._default_compilers)
DEFAULT_COMPILER_MAPPING = [
    ('cygwin.*', 'unix'),
    ('zos', 'zos'),
    ('posix', 'unix'),
    ('nt', 'msvc'),
]

# Common compiler families
COMPILER_FAMILIES = {
    'gcc': ['unix', 'cygwin', 'mingw32'],
    'msvc': ['msvc'],
    'borland': ['bcpp'],
    'ibm': ['zos'],
}

# Platform-specific information
PLATFORM_INFO = {
    'linux': {
        'default_compiler': 'unix',
        'available_compilers': ['unix', 'mingw32'],
        'typical_executables': ['gcc', 'g++', 'clang', 'clang++'],
    },
    'darwin': {
        'default_compiler': 'unix',
        'available_compilers': ['unix'],
        'typical_executables': ['clang', 'clang++', 'gcc', 'g++'],
    },
    'win32': {
        'default_compiler': 'msvc',
        'available_compilers': ['msvc', 'mingw32', 'cygwin', 'bcpp'],
        'typical_executables': ['cl.exe', 'gcc.exe', 'g++.exe'],
    },
    'cygwin': {
        'default_compiler': 'unix',
        'available_compilers': ['unix', 'cygwin', 'mingw32'],
        'typical_executables': ['gcc', 'g++'],
    },
}


def get_available_compiler_types() -> List[str]:
    """
    Get all available compiler types that can be instantiated.
    
    Returns:
        List of compiler type strings that are actually available.
    """
    available = []
    for comp_type in COMPILER_TYPES:
        try:
            ccompiler.new_compiler(compiler=comp_type)
            available.append(comp_type)
        except Exception:
            # Compiler not available on this system
            pass
    return available


def get_default_compiler_for_platform(platform: Optional[str] = None) -> str:
    """
    Get the default compiler for a given platform.
    
    Args:
        platform: Platform name (e.g., 'posix', 'nt'). If None, uses current platform.
        
    Returns:
        Default compiler type string for the platform.
    """
    if platform is None:
        return ccompiler.get_default_compiler()

    # Match against the default compiler mapping
    for pattern, compiler in DEFAULT_COMPILER_MAPPING:
        if re.match(pattern, platform):
            return compiler

    # Fallback to unix for unknown platforms
    return 'unix'


def get_compiler_info(compiler_type: str) -> Dict[str, Any]:
    """
    Get detailed information about a specific compiler type.
    
    Args:
        compiler_type: The compiler type string.
        
    Returns:
        Dictionary containing compiler information.
    """
    info = {
        'type': compiler_type,
        'description': COMPILER_DESCRIPTIONS.get(compiler_type, 'Unknown compiler'),
        'available': False,
        'class_name': None,
        'module': None,
        'family': None,
    }

    # Determine compiler family
    for family, compilers in COMPILER_FAMILIES.items():
        if compiler_type in compilers:
            info['family'] = family
            break

    # Try to instantiate the compiler to get more info
    try:
        comp = ccompiler.new_compiler(compiler=compiler_type)
        info['available'] = True
        info['class_name'] = comp.__class__.__name__
        info['module'] = comp.__class__.__module__

        # Get additional attributes if available
        if hasattr(comp, 'compiler_type'):
            info['compiler_type_attr'] = comp.compiler_type
        if hasattr(comp, 'exe_extension'):
            info['exe_extension'] = comp.exe_extension
        if hasattr(comp, 'obj_extension'):
            info['obj_extension'] = comp.obj_extension
        if hasattr(comp, 'static_lib_extension'):
            info['static_lib_extension'] = comp.static_lib_extension
        if hasattr(comp, 'shared_lib_extension'):
            info['shared_lib_extension'] = comp.shared_lib_extension

    except Exception as e:
        info['error'] = str(e)

    return info


def get_all_compiler_info() -> Dict[str, Dict[str, Any]]:
    """
    Get information about all known compiler types.
    
    Returns:
        Dictionary mapping compiler types to their information.
    """
    return {comp_type: get_compiler_info(comp_type) for comp_type in COMPILER_TYPES}


def get_platform_info(platform: Optional[str] = None) -> Dict[str, Any]:
    """
    Get platform-specific compiler information.
    
    Args:
        platform: Platform name. If None, uses current platform.
        
    Returns:
        Dictionary containing platform information.
    """
    if platform is None:
        platform = sys.platform

    current_platform = get_platform()

    info = {
        'sys_platform': platform,
        'distutils_platform': current_platform,
        'default_compiler': get_default_compiler_for_platform(platform),
        'available_compilers': get_available_compiler_types(),
    }

    # Add platform-specific information if available
    for plat_key, plat_info in PLATFORM_INFO.items():
        if platform.startswith(plat_key):
            info.update(plat_info)
            break

    return info


def is_compiler_available(compiler_type: str) -> bool:
    """
    Check if a specific compiler type is available on the current system.
    
    Args:
        compiler_type: The compiler type string.
        
    Returns:
        True if the compiler is available, False otherwise.
    """
    try:
        ccompiler.new_compiler(compiler=compiler_type)
        return True
    except Exception:
        return False


def get_compiler_by_executable(executable: str) -> Optional[str]:
    """
    Try to determine the compiler type based on the executable name.
    
    Args:
        executable: The compiler executable name or path.
        
    Returns:
        The most likely compiler type, or None if unknown.
    """
    exe_name = os.path.basename(executable).lower()

    # Remove common extensions
    if exe_name.endswith('.exe'):
        exe_name = exe_name[:-4]

    # Common mappings
    if exe_name in ['cl', 'msvc']:
        return 'msvc'
    elif exe_name in ['gcc', 'g++', 'cc', 'c++']:
        if sys.platform == 'win32':
            return 'mingw32'  # Assume MinGW on Windows
        else:
            return 'unix'
    elif exe_name in ['clang', 'clang++']:
        return 'unix'
    elif exe_name.startswith('bcc'):
        return 'bcpp'
    elif 'xlc' in exe_name or 'xlC' in exe_name:
        return 'zos'
    elif 'cygwin' in exe_name or exe_name.endswith('-gcc'):
        return 'cygwin'

    return None


def print_compiler_summary():
    """Print a summary of all compiler information."""
    print("=== Distutils Compiler Summary ===")
    print()

    # Platform info
    platform_info = get_platform_info()
    print(f"Current Platform: {platform_info['sys_platform']}")
    print(f"Distutils Platform: {platform_info['distutils_platform']}")
    print(f"Default Compiler: {platform_info['default_compiler']}")
    print()

    # Available compilers
    print("Available Compilers:")
    for comp_type in get_available_compiler_types():
        info = get_compiler_info(comp_type)
        print(f"  {comp_type}: {info['description']}")
    print()

    # All compiler info
    print("All Known Compilers:")
    all_info = get_all_compiler_info()
    for comp_type, info in all_info.items():
        status = "✓" if info['available'] else "✗"
        print(f"  {status} {comp_type}: {info['description']}")
        if info['available']:
            print(f"      Class: {info['class_name']} ({info['module']})")
        elif 'error' in info:
            print(f"      Error: {info['error']}")
    print()
