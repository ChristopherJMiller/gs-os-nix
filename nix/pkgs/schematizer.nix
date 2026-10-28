# SPDX-License-Identifier: GPL-3.0-or-later
#
# schematizer -- schematic generation, split out of SKiDL in 3.0.0. Pure Python;
# SKiDL imports it lazily when asked for a schematic.
{ lib
, buildPythonPackage
, fetchPypi
, setuptools
, simp-sexp
}:

buildPythonPackage rec {
  pname = "schematizer";
  version = "0.2.0";
  pyproject = true;

  src = fetchPypi {
    inherit pname version;
    hash = "sha256-2C07cNiP+hDazbNbEUJ4EUWlAMr/kJ6q04J/0dMChCQ=";
  };

  build-system = [ setuptools ];
  dependencies = [ simp-sexp ];

  pythonImportsCheck = [ "schematizer" ];

  meta = {
    description = "Schematic generation for SKiDL";
    homepage = "https://pypi.org/project/schematizer/";
    license = lib.licenses.mit;
  };
}
