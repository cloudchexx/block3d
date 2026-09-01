import sys

if "--cli" in sys.argv:
    sys.argv.remove("--cli")
    from .cli import main
    main()
else:
    from .gui import GeneratorGUI
    app = GeneratorGUI()
    app.mainloop()
