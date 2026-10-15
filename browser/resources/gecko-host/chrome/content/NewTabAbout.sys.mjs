import {NEW_TAB_DOCUMENT} from 'chrome://dng/content/NewTabDocument.sys.mjs';
const CID=Components.ID('{f204b376-34ec-4df9-960a-561a1b97d71f}');
const CONTRACT='@mozilla.org/network/protocol/about;1?what=dalinira-newtab';
export function installNewTabAbout(){
 const registrar=Components.manager.QueryInterface(Ci.nsIComponentRegistrar);
 if(registrar.isContractIDRegistered(CONTRACT))return;
 const module={QueryInterface:ChromeUtils.generateQI(['nsIAboutModule']),
  getURIFlags(){return Ci.nsIAboutModule.URI_SAFE_FOR_UNTRUSTED_CONTENT|Ci.nsIAboutModule.ALLOW_SCRIPT|Ci.nsIAboutModule.URI_CAN_LOAD_IN_CHILD|Ci.nsIAboutModule.URI_MUST_LOAD_IN_CHILD;},
  getChromeURI(){return null;},
  newChannel(uri,loadInfo){
   if(uri.spec!=='about:dalinira-newtab')throw Components.Exception('Unknown page',Cr.NS_ERROR_MALFORMED_URI);
   const stream=Cc['@mozilla.org/io/string-input-stream;1'].createInstance(Ci.nsIStringInputStream);stream.setUTF8Data(NEW_TAB_DOCUMENT);
   const input=Cc['@mozilla.org/network/input-stream-channel;1'].createInstance(Ci.nsIInputStreamChannel);input.setURI(uri);input.contentStream=stream;
   const channel=input.QueryInterface(Ci.nsIChannel);channel.loadInfo=loadInfo;channel.originalURI=uri;channel.owner=null;channel.contentType='text/html';channel.contentCharset='UTF-8';return channel;
  }};
 const factory={QueryInterface:ChromeUtils.generateQI(['nsIFactory']),createInstance(iid){return module.QueryInterface(iid);}};
 registrar.registerFactory(CID,'DaliNira unprivileged New Tab',CONTRACT,factory);
}
