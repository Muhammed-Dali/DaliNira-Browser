/* Application policy callbacks for the standalone DaliNira Gecko Host.
 * The stock GRE EnterprisePoliciesManager loads this application module.
 * Only the update-disable policy is supported by this prototype.
 */
export const Policies = {
  DisableAppUpdate: {
    onBeforeAddons(manager, enabled) {
      if (enabled) {
        manager.disallowFeature("appUpdate");
      }
    },
  },
};
