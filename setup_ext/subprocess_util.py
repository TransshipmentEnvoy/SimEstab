def safe_decode_stdout(stdout_bytes: bytes) -> str:
    """
    Safely decode stdout bytes with auto-detection of encoding.
    Falls back to utf-8 with error handling if detection fails.
    """
    if not stdout_bytes:
        return ""

    try:
        # Try to import chardet for encoding detection
        import chardet
        detected = chardet.detect(stdout_bytes)
        if detected and detected.get('encoding') and detected.get('confidence', 0) > 0.5:
            encoding = detected['encoding']
            try:
                return stdout_bytes.decode(encoding)
            except (UnicodeDecodeError, LookupError):
                pass
    except ImportError:
        # chardet not available, continue with fallback
        pass

    # Fallback to common encodings
    for encoding in ['utf-8', 'cp1252', 'latin1']:
        try:
            return stdout_bytes.decode(encoding)
        except UnicodeDecodeError:
            continue

    # Last resort: decode with utf-8 and replace errors
    return stdout_bytes.decode('utf-8', errors='replace')
