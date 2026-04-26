import sys

from smart_todo import SmartTodoApp


def configure_console_encoding() -> None:
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")


def main() -> None:
    configure_console_encoding()
    app = SmartTodoApp()
    app.run()


if __name__ == "__main__":
    main()
