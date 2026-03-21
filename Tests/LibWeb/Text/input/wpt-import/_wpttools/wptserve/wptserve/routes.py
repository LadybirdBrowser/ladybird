from . import handlers
from .router import any_method
routes = [(any_method, "*.py", handlers.python_script_handler),
          ("GET", "*.asis", handlers.as_is_handler),
          ("GET", "*", handlers.file_handler),
          ]
