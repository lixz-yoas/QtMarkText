import { V as requireComponents$1, W as requireDependencies, y as getDefaultExportFromCjs } from "./index-B-PE276Q.js";
function _mergeNamespaces(n, m) {
  for (var i = 0; i < m.length; i++) {
    const e = m[i];
    if (typeof e !== "string" && !Array.isArray(e)) {
      for (const k in e) {
        if (k !== "default" && !(k in n)) {
          const d = Object.getOwnPropertyDescriptor(e, k);
          if (d) {
            Object.defineProperty(n, k, d.get ? d : {
              enumerable: true,
              get: () => e[k]
            });
          }
        }
      }
    }
  }
  return Object.freeze(Object.defineProperty(n, Symbol.toStringTag, { value: "Module" }));
}
function commonjsRequire(path) {
  throw new Error('Could not dynamically require "' + path + '". Please configure the dynamicRequireTargets or/and ignoreDynamicRequires option of @rollup/plugin-commonjs appropriately for this require call to work.');
}
var components_1;
var hasRequiredComponents;
function requireComponents() {
  if (hasRequiredComponents) return components_1;
  hasRequiredComponents = 1;
  const components = requireComponents$1();
  const getLoader = requireDependencies();
  const loadedLanguages = /* @__PURE__ */ new Set();
  function loadLanguages(languages) {
    if (languages === void 0) {
      languages = Object.keys(components.languages).filter((l) => l != "meta");
    } else if (!Array.isArray(languages)) {
      languages = [languages];
    }
    const loaded = [...loadedLanguages, ...Object.keys(Prism.languages)];
    getLoader(components, languages, loaded).load((lang) => {
      if (!(lang in components.languages)) {
        if (!loadLanguages.silent) {
          console.warn("Language does not exist: " + lang);
        }
        return;
      }
      const pathToLanguage = "./prism-" + lang;
      delete require.cache[require.resolve(pathToLanguage)];
      delete Prism.languages[lang];
      commonjsRequire(pathToLanguage);
      loadedLanguages.add(lang);
    });
  }
  loadLanguages.silent = false;
  components_1 = loadLanguages;
  return components_1;
}
var componentsExports = requireComponents();
const index = /* @__PURE__ */ getDefaultExportFromCjs(componentsExports);
const index$1 = /* @__PURE__ */ _mergeNamespaces({
  __proto__: null,
  default: index
}, [componentsExports]);
export {
  index$1 as i
};
