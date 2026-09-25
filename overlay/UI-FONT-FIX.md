# UI font compatibility fix

The original ProsperoRadio bitmap font engine resolves an exact `(family, style, weight, size)` tuple. The shipped Montserrat faces are 20, 24, 28, 32, 36, 40 and 48 px at weight 500.

Version `01.000.007` removes unsupported sizes/weights from the modernization overlay. This is why earlier builds could render panels, borders and colors while producing little or no text: the UI requested font tuples that the runtime did not load.
