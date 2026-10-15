export class DaliNiraNewTabParent extends JSWindowActorParent {
 receiveMessage(message){
  try{return this.browsingContext.top.embedderElement.ownerDocument.defaultView.DngNewTab.receive(this,message);}
  catch(e){return {ok:false};}
 }
}
