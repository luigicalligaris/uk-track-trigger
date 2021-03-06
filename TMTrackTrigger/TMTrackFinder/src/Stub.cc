#include "Geometry/TrackerGeometryBuilder/interface/StackedTrackerGeometry.h"
//#include "SimTracker/TrackTriggerAssociation/interface/TTStubAssociationMap.h"
#include "DataFormats/Math/interface/deltaPhi.h"
#include "FWCore/Utilities/interface/Exception.h"

#include "TMTrackTrigger/TMTrackFinder/interface/Stub.h"
#include "TMTrackTrigger/TMTrackFinder/interface/DataCorrection.h"
#include "TMTrackTrigger/TMTrackFinder/interface/TP.h"

#include <iostream>

using namespace std;

//=== Store useful info about this stub.

Stub::Stub(TTStubRef ttStubRef, unsigned int index_in_vStubs, const Settings* settings, 
           const StackedTrackerGeometry*  stackedGeometry) : 
//   TTStubRef(ttStubRef), 
  settings_(settings), 
  index_in_vStubs_(index_in_vStubs), 
  digitalStub_(settings),
  digitizedForGPinput_(false), // notes that stub has not yet been digitized for GP input.
  digitizedForHTinput_(false), // notes that stub has not yet been digitized for HT input.
	cmssswTTStubRef_(ttStubRef)
{
  // Get coordinates of stub.
  const TTStub<Ref_PixelDigi_> *ttStubP = ttStubRef.get(); 

  // Actually seems to be taken from cluster in inner of two sensors making up stub?
  GlobalPoint pos = stackedGeometry->findGlobalPosition(ttStubP);
  phi_ = pos.phi();
  r_   = pos.perp();
  z_   = pos.z();

  if (r_ < settings_->trackerInnerRadius() || r_ > settings_->trackerOuterRadius() || fabs(z_) > settings_->trackerHalfLength()) throw cms::Exception("Stub: Stub found outside assumed tracker volume. Please update tracker dimensions specified in Settingsm.h!")<<" r="<<r_<<" z="<<z_<<endl;

  // Note detector module containing stub.
  StackedTrackerDetId stDetId = ttStubRef->getDetId();

  // Set info about the module this stub is in
  this->setModuleInfo(stackedGeometry, stDetId);

  // Uncertainty in stub coordinates due to strip length in case of 2S modules.
  rErr_ = 0.;
  zErr_ = 0.;

  if (barrel_) {
    zErr_ = 0.5*stripLength_;
  } else {
    rErr_ = 0.5*stripLength_; 
  }

  // Get the coordinates of the two clusters that make up this stub, measured in units of strip pitch, and measured
  // in the local frame of the sensor. They have a granularity  of 0.5*pitch.
  for (unsigned int iClus = 0; iClus <= 1; iClus++) { // Loop over two clusters in stub.  
    localU_cluster_[iClus] = ttStubP->getClusterRef(iClus)->findAverageLocalCoordinates().x();
    localV_cluster_[iClus] = ttStubP->getClusterRef(iClus)->findAverageLocalCoordinates().y();
  }

  // Get location of stub in module in units of strip number (or pixel number along finest granularity axis).
  // Range from 0 to (nStrips - 1) inclusive.
  // N.B. Since iphi is integer, this degrades the granularity by a factor 2. This seems silly, but track fit wants it.
  iphi_ = localU_cluster_[0]; // granularity 1*strip (unclear why we want to degrade it ...)

  // Get stub bend (i.e. displacement between two hits in stub in units of strip pitch).
  float bend = ttStubRef->getTriggerBend();
  if (stDetId.isEndcap() && pos.z() > 0) bend *= -1;
  // Note this raw bend, which will be available inside front-end chip.
  bendInFrontend_ = bend;

  // Degrade stub bend resolution if required.
  float degradedBend;         // degraded bend
  bool rejectStub;            // indicates if bend is outside window assumed in DataCorrection.h
  unsigned int numMergedBend; // Number of bend values merged into the single degraded one.
  this->degradeResolution(bend, stDetId,
			  degradedBend, rejectStub, numMergedBend);
  if (settings->bendResReduced()) {
    bend = degradedBend;
    numMergedBend_ = numMergedBend;
  } else {
    numMergedBend_ = 1;
  }

  bend_ = bend;

  // Estimate track Pt and phi0 based on stub bend info, and angle in r-phi projection of stub direction to sensor plane.
  float pitch = stripPitch_; // pitch of strip sensor (or of pixel sensor in high granularity direction).
  float sensorSpacing = barrel_ ? (moduleMaxR_ - moduleMinR_) : (moduleMaxZ_ - moduleMinZ_);
  pitchOverSep_ = pitch/sensorSpacing;
  // IRT - use stub (r,z) instead of module (r,z). Logically correct but has negligable effect on results.
  //float deltaR = barrel_ ? sensorSpacing : sensorSpacing*R0/fabs(Z0) ;
  float deltaR = barrel_ ? sensorSpacing : sensorSpacing*r_/fabs(z_) ; // Diff in radius of coords where track crosses the two sensors.
  dphiOverBend_ = pitch/deltaR;
  dphi_ = bend_ * dphiOverBend();

  // Fill frontendPass_ flag, indicating if frontend readout electronics will output this stub.
  this->setFrontend(rejectStub); 

  // Calculate bin range along q/Pt axis of r-phi Hough transform array consistent with bend of this stub.
  this->calcQoverPtrange();

  // Initialize class used to produce digital version of stub, with original stub parameters pre-digitization.
  digitalStub_.init(phi_, r_, z_, dphi(), this->rhoParameter(), min_qOverPt_bin_, max_qOverPt_bin_, layerId_, this->layerIdReduced(), bend_, stripPitch_, sensorSpacing);
}

//=== Calculate bin range along q/Pt axis of r-phi Hough transform array consistent with bend of this stub.

void Stub::calcQoverPtrange() {
  // First determine bin range along q/Pt axis of HT array 
  const int nbinsPt = (int) settings_->houghNbinsPt(); // Use "int" as nasty things happen if multiply "int" and "unsigned int".
  const int min_array_bin = 0;
  const int max_array_bin = nbinsPt - 1;
  // Now calculate range of q/Pt bins allowed by bend filter.
  float qOverPtMin = this->qOverPtOverBend() * (this->bend() - this->bendRes());
  float qOverPtMax = this->qOverPtOverBend() * (this->bend() + this->bendRes());
  const float houghMaxInvPt = 1./settings_->houghMinPt();
  const float qOverPtBinSize = (2. * houghMaxInvPt)/settings_->houghNbinsPt();
  // Convert to bin number along q/Pt axis of HT array.
  // N.B. The terms involving "0.5" here have the effect that the cell is accepted if the q/Pt at its centre is
  // consistent with the stub bend. This gives the same behaviour for the "daisy chain" firmware, which uses
  // this bin range, and for the systolic/2-c-bin firmwares which instead use the calculation in HTcell::bendFilter().
  // If you choose to remove the "0.5" terms here, which loosens the bend filter cut, then I recommend that you 
  // tighten up the value of the "BendResolution" config parameter by about 0.05 to compensate.
  // Decision to remove them taken in softare & GP firmware on 9th August 2016.
  //int min_bin = std::floor(  0.5 + (qOverPtMin + houghMaxInvPt)/qOverPtBinSize);
  //int max_bin = std::floor( -0.5 + (qOverPtMax + houghMaxInvPt)/qOverPtBinSize);
  int min_bin = std::floor((qOverPtMin + houghMaxInvPt)/qOverPtBinSize);
  int max_bin = std::floor((qOverPtMax + houghMaxInvPt)/qOverPtBinSize);
  // Limit it to range of HT array.
  min_bin = max(min_bin, min_array_bin);
  max_bin = min(max_bin, max_array_bin);
  // If min_bin > max_bin at this stage, it means that the Pt estimated from the bend is below the range we wish to find tracks in.
  // Keep min_bin > max_bin, so such stubs can be identified, but set both variables to values inside the allowed range.
  if (min_bin > max_bin) {
    min_bin = max_array_bin;
    max_bin = min_array_bin;
    //if (frontendPass_) throw cms::Exception("Stub: m bin calculation found low Pt stub not killed by FE electronics cuts")<<qOverPtMin<<" "<<qOverPtMax<<endl;
  }
  min_qOverPt_bin_ = (unsigned int) min_bin;
  max_qOverPt_bin_ = (unsigned int) max_bin;
}

//=== Digitize stub for input to Geographic Processor, with digitized phi coord. measured relative to closest phi sector.
//=== (This approximation is valid if their are an integer number of digitisation bins inside each phi octant).
//=== However, you should also call digitizeForHTinput() before accessing digitized stub data, even if you only care about that going into GP! Otherwise, you will not identify stubs assigned to more than one octant.

void Stub::digitizeForGPinput(unsigned int iPhiSec) {
  if (settings_->enableDigitize()) {

    // Save CPU by not redoing digitization if stub was already digitized for this phi sector.
    if ( ! (digitizedForGPinput_ && digitalStub_.iGetOctant(iPhiSec) == digitalStub_.iDigi_Octant()) ) {

      // Digitize
      digitalStub_.makeGPinput(iPhiSec);

      // Replace stub coordinates with those degraded by digitization process.
      phi_  = digitalStub_.phi();
      r_    = digitalStub_.r();
      z_    = digitalStub_.z();
      bend_ = digitalStub_.bend();

      // If the Stub class contains any data members that are not input to the GP, but are derived from variables that
      // are, then be sure to update these here too, unless Stub.h uses the check*() functions to declare them invalid. 
      // - currently none.

      // Note that stub has been digitized for GP input
      digitizedForGPinput_ = true;
    }
    digitizedForHTinput_ = false;
  }
}

//=== Digitize stub for input to Hough transform, with digitized phi coord. measured relative to specified phi sector.

void Stub::digitizeForHTinput(unsigned int iPhiSec) {

  if (settings_->enableDigitize()) {

    // Save CPU by not redoing digitization if stub was already digitized for this phi sector.
    if ( ! (digitizedForHTinput_ && iPhiSec == digitalStub_.iDigi_PhiSec()) ) {

      // Call digitization for GP in case not already done. (Needed for variables that are common to GP & HT).
      this->digitizeForGPinput(iPhiSec);

      // Digitize
      digitalStub_.makeHTinput(iPhiSec);

      // Since GP and HT use same digitisation in r and z, don't bother updating their values.
      // (Actually, the phi digitisation boundaries also match, except for systolic array, so could skip updating phi too).

      // Replace stub coordinates and bend with those degraded by digitization process.
      phi_  = digitalStub_.phi();

      // Variables dphi & rho are not used with daisy-chain firmware.
      if (settings_->firmwareType() != 1) {
	dphi_ = digitalStub_.dphi();
	float rho  = digitalStub_.rho();
	this->setRhoParameter(rho);
      }

      // If the Stub class contains any data members that are not input to the HT, but are derived from variables that
      // are, then be sure to update these here too, unless Stub.h uses the check*() functions to declare them invalid. 

      if (settings_->firmwareType() != 1) {
	// Recalculate bin range along q/Pt axis of r-phi Hough transform array consistent with bend of this stub,
	// since it depends on dphi which has now been digitized. Not needed with daisy-chain firmware, since this range
	// is transmitted to HT hardware along optical link.
	this->calcQoverPtrange();
      }

      // Note that stub has been digitized.
      digitizedForHTinput_ = true;
    }
  }
}

//===  Restore stub to pre-digitized state. i.e. Undo what function digitize() did.

void Stub::reset_digitize() {
  if (settings_->enableDigitize()) {
    // Save CPU by not undoing digitization if stub was not already digitized.
    if (digitizedForGPinput_ || digitizedForHTinput_) {

      // Replace stub coordinates and bend with original coordinates stored prior to any digitization.
      phi_  = digitalStub_.orig_phi();
      r_    = digitalStub_.orig_r();
      z_    = digitalStub_.orig_z();
      bend_ = digitalStub_.orig_bend();

      // Variables dphi & rho are not used with daisy-chain firmware.
      if (settings_->firmwareType() != 1) {
	dphi_ = digitalStub_.orig_dphi();
	float rho  = digitalStub_.orig_rho();
	this->setRhoParameter(rho);
      }

      // If the Stub class contains any data members that are not input to the GP or HT, but are derived from 
      // variables that are, then be sure to update these here too.

      if (settings_->firmwareType() != 1) {
	// Recalculate bin range along q/Pt axis of r-phi Hough transform array consistent with bend of this stub,
	// since it depends on dphi which is no longer digitized. Not needed with daisy-chain firmware, since this range
	// is transmitted to HT hardware along optical link.
	this->calcQoverPtrange();
      }

      // Note that stub is (no logner) digitized
      digitizedForGPinput_ = false;
      digitizedForHTinput_ = false;
    }
  }
}

//=== Degrade assumed stub bend resolution.
//=== Also return boolean indicating if stub bend was outside assumed window, so stub should be rejected
//=== and return an integer indicating how many values of bend are merged into this single one.

void Stub::degradeResolution(float  bend, const  StackedTrackerDetId& stDetId,
	float& degradedBend, bool& reject, unsigned int& num) const
{
  if (barrel_)
  {
    unsigned int layer = stDetId.iLayer();
    DataCorrection::ConvertBarrelBend( bend, layer,
				       degradedBend, reject, num);
  } else {
    unsigned int ring = stDetId.iRing();
    DataCorrection::ConvertEndcapBend( bend, ring,
				       degradedBend, reject, num);
  }
}


//=== Set flag indicating if stub will be output by front-end readout electronics 
//=== (where we can reconfigure the stub window size and rapidity cut).
//=== Argument indicates if stub bend was outside window size encoded in DataCorrection.h
//=== Note that this should run on quantities as available inside front-end chip, which are not
//=== degraded by loss of bits or digitisation.

void Stub::setFrontend(bool rejectStub) {
  frontendPass_ = true; // Did stub pass cuts applied in front-end chip
  stubFailedDataCorrWindow_ = false; // Did it only fail cuts corresponding to windows encoded in DataCorrection.h?
  // Don't use stubs at large eta, since it is impossible to form L1 tracks from them, so they only contribute to combinatorics.
  if ( fabs(this->eta()) > settings_->maxStubEta() ) frontendPass_ = false;
  // Don't use stubs whose Pt is significantly below the Pt cut used in the L1 tracking, allowing for uncertainty in q/Pt due to stub bend resolution.
  if (settings_->killLowPtStubs()) {
    const float qOverPtCut = 1./settings_->houghMinPt();
    // Apply this cut in the front-end electronics.
    if (fabs(this->bendInFrontend()) - this->bendResInFrontend() > qOverPtCut/this->qOverPtOverBend()) frontendPass_ = false;
    // Reapply the same cut using the degraded bend information available in the off-detector electronics.
    // The reason is  that the bend degredation can move the Pt below the Pt cut, making the stub useless to the off-detector electronics.
    if (fabs(this->bend())           - this->bendRes()           > qOverPtCut/this->qOverPtOverBend()) frontendPass_ = false;
  } 
  // Don't use stubs whose bend is outside the window encoded into DataCorrection.h
  if (rejectStub) {
    if (frontendPass_) stubFailedDataCorrWindow_ = true;
    frontendPass_ = false;
  }
}

//=== Note which tracking particle(s), if any, produced this stub.
//=== The 1st argument is a map relating TrackingParticles to TP.

void Stub::fillTruth(const map<edm::Ptr< TrackingParticle >, const TP* >& translateTP, edm::Handle<TTStubAssMap> mcTruthTTStubHandle, edm::Handle<TTClusterAssMap> mcTruthTTClusterHandle){

	// TODO: remove me
//   TTStubRef ttStubRef(*this); // Cast to base class

  //--- Fill assocTP_ info. If both clusters in this stub were produced by the same single tracking particle, find out which one it was.

//   bool genuine =  mcTruthTTStubHandle->isGenuine(ttStubRef); // Same TP contributed to both clusters?
  bool genuine =  mcTruthTTStubHandle->isGenuine( cmssswTTStubRef_ ); // Same TP contributed to both clusters?
  assocTP_ = nullptr;

  // Require same TP contributed to both clusters.
  if ( genuine ) {
//     edm::Ptr< TrackingParticle > tpPtr = mcTruthTTStubHandle->findTrackingParticlePtr(ttStubRef);
    edm::Ptr< TrackingParticle > tpPtr = mcTruthTTStubHandle->findTrackingParticlePtr( cmssswTTStubRef_ );
    if (translateTP.find(tpPtr) != translateTP.end()) {
      assocTP_ = translateTP.at(tpPtr);
      // N.B. Since not all tracking particles are stored in InputData::vTPs_, sometimes no match will be found.
    }
  }

  // Fill assocTPs_ info.

  if (settings_->stubMatchStrict()) {

    // We consider only stubs in which this TP contributed to both clusters.
    if (assocTP_ != nullptr) assocTPs_.insert(assocTP_);

  } else {

    // We consider stubs in which this TP contributed to either cluster.

    for (unsigned int iClus = 0; iClus <= 1; iClus++) { // Loop over both clusters that make up stub.
       const TTClusterRef& ttClusterRef = cmssswTTStubRef_->getClusterRef(iClus);

      // Now identify all TP's contributing to either cluster in stub.
      vector< edm::Ptr< TrackingParticle > > vecTpPtr = mcTruthTTClusterHandle->findTrackingParticlePtrs(ttClusterRef);

      for (edm::Ptr< TrackingParticle> tpPtr : vecTpPtr) {
	if (translateTP.find(tpPtr) != translateTP.end()) {
	  assocTPs_.insert( translateTP.at(tpPtr) );
	  // N.B. Since not all tracking particles are stored in InputData::vTPs_, sometimes no match will be found.
	}
      }
    }
  }

  //--- Also note which tracking particles produced the two clusters that make up the stub

  for (unsigned int iClus = 0; iClus <= 1; iClus++) { // Loop over both clusters that make up stub.
    const TTClusterRef& ttClusterRef = cmssswTTStubRef_->getClusterRef(iClus);

    bool genuineCluster =  mcTruthTTClusterHandle->isGenuine(ttClusterRef); // Only 1 TP made cluster?
    assocTPofCluster_[iClus] = nullptr;

    // Only consider clusters produced by just one TP.
    if ( genuineCluster ) {
      edm::Ptr< TrackingParticle > tpPtr = mcTruthTTClusterHandle->findTrackingParticlePtr(ttClusterRef);

      if (translateTP.find(tpPtr) != translateTP.end()) {
	assocTPofCluster_[iClus] = translateTP.at(tpPtr);
	// N.B. Since not all tracking particles are stored in InputData::vTPs_, sometimes no match will be found.
      }
    }
  }

  // Sanity check - is truth info of stub consistent with that of its clusters?
  // Commented this out, as it throws errors for unknown reason with iErr=1. Apparently, "genuine" stubs can be composed of two clusters that are
  // not "genuine", providing that one of the TP that contributed to each cluster was the same.
  /*
  unsigned int iErr = 0;
  if (this->genuine()) { // Stub matches truth particle
    if ( ! ( this->genuineCluster()[0] && (this->assocTPofCluster()[0] == this->assocTPofCluster()[1]) ) ) iErr = 1;
  } else {
    if ( ! ( ! this->genuineCluster()[0] || (this->assocTPofCluster()[0] != this->assocTPofCluster()[1]) )  ) iErr = 2;
  }
  if (iErr > 0) {
    cout<<" DEBUGA "<<(this->assocTP() == nullptr)<<endl;
    cout<<" DEBUGB "<<(this->assocTPofCluster()[0] == nullptr)<<" "<<(this->assocTPofCluster()[1] == nullptr)<<endl;
    cout<<" DEBUGC "<<this->genuineCluster()[0]<<" "<<this->genuineCluster()[1]<<endl;
    if (this->assocTPofCluster()[0] != nullptr) cout<<" DEBUGD "<<this->assocTPofCluster()[0]->index()<<endl;
    if (this->assocTPofCluster()[1] != nullptr) cout<<" DEBUGE "<<this->assocTPofCluster()[1]->index()<<endl;
    //    throw cms::Exception("Stub: Truth info of stub & its clusters inconsistent!")<<iErr<<endl;
  }
  */
}

//=== Estimated phi angle at which track crosses a given radius rad, based on stub bend info. Also estimate uncertainty on this angle due to endcap 2S module strip length.
//=== N.B. This is identical to Stub::beta() if rad=0.

pair <float, float> Stub::trkPhiAtR(float rad) const { 
  float rStubMax = r_ + rErr_; // Uncertainty in radial stub coordinate due to strip length.
  float rStubMin = r_ - rErr_;
  float trkPhi1 = (phi_ + dphi()*(1. - rad/rStubMin));
  float trkPhi2 = (phi_ + dphi()*(1. - rad/rStubMax));
  float trkPhi    = 0.5*    (trkPhi1 + trkPhi2);
  float errTrkPhi = 0.5*fabs(trkPhi1 - trkPhi2); 
  return pair<float, float>(trkPhi, errTrkPhi);
}


//=== Note if stub is a crazy distance from the tracking particle trajectory that produced it.
//=== If so, it was probably produced by a delta ray.

bool Stub::crazyStub() const {

  bool crazy;
  if (assocTP_ == nullptr) {
    crazy = false; // Stub is fake, but this is not crazy. It happens ...
  } else {
    // Stub was produced by TP. Check it lies not too far from TP trajectory.
    crazy = fabs( reco::deltaPhi(phi_, assocTP_->trkPhiAtStub( this )) )  >  settings_->crazyStubCut();
  } 
  return crazy;
}

//=== Get reduced layer ID (in range 1-7), which can be packed into 3 bits so simplifying the firmware).

unsigned int Stub::layerIdReduced() const {
  // Don't bother distinguishing two endcaps, as no track can have stubs in both.
  unsigned int lay = (layerId_ < 20) ? layerId_ : layerId_ - 10; 

  // No genuine track can have stubs in both barrel layer 6 and endcap disk 11 etc., so merge their layer IDs.
  // WARNING: This is tracker geometry dependent, so may need changing in future ...
  if (lay == 6) lay = 11; 
  if (lay == 5) lay = 12; 
  if (lay == 4) lay = 13; 
  if (lay == 3) lay = 15; 
  // At this point, the reduced layer ID can have values of 1, 2, 11, 12, 13, 14, 15. So correct to put in range 1-7.
  if (lay > 10) lay -= 8;

  if (lay < 1 || lay > 7) throw cms::Exception("Stub: Reduced layer ID out of expected range");

  return lay;
}


//=== Set info about the module that this stub is in.

void Stub::setModuleInfo(const StackedTrackerGeometry* stackedGeometry, const StackedTrackerDetId& stDetId) {

  // Get unique identifier of this module.
  idDet_ = stDetId();

  // Get min & max (r,phi,z) coordinates of the centre of the two sensors containing this stub.
  const GeomDetUnit* det0 = stackedGeometry->idToDetUnit( stDetId, 0 );
  const GeomDetUnit* det1 = stackedGeometry->idToDetUnit( stDetId, 1 );
  float R0 = det0->position().perp();
  float R1 = det1->position().perp();
  float PHI0 = det0->position().phi();
  float PHI1 = det1->position().phi();
  float Z0 = det0->position().z();
  float Z1 = det1->position().z();
  moduleMinR_   = std::min(R0,R1);
  moduleMaxR_   = std::max(R0,R1);
  moduleMinPhi_ = std::min(PHI0,PHI1);
  moduleMaxPhi_ = std::max(PHI0,PHI1);
  moduleMinZ_   = std::min(Z0,Z1);
  moduleMaxZ_   = std::max(Z0,Z1);

  // Note if module is PS or 2S, and whether in barrel or endcap.
  psModule_   = stackedGeometry->isPSModule(stDetId);
  barrel_     = stDetId.isBarrel();

  //  cout<<"DEBUG STUB "<<barrel_<<" "<<psModule_<<"  sep(r,z)=( "<<moduleMaxR_ - moduleMinR_<<" , "<<moduleMaxZ_ - moduleMinZ_<<" )    stub(r,z)=( "<<0.5*(moduleMaxR_ + moduleMinR_) - r_<<" , "<<0.5*(moduleMaxZ_ + moduleMinZ_) - z_<<" )"<<endl;

  // Encode layer ID.
  if (barrel_) {
    layerId_ = stDetId.iLayer(); // barrel layer 1-6 encoded as 1-6
  } else {
    layerId_ = 10*stDetId.iSide() + stDetId.iDisk(); // endcap layer 1-5 encoded as 11-15 (endcap A) or 21-25 (endcapB)
  }

  // Note module ring in endcap
  endcapRing_ = barrel_  ?  0  :  stDetId.iRing();

  // Get sensor strip or pixel pitch using innermost sensor of pair.
  const PixelGeomDetUnit* unit = reinterpret_cast<const PixelGeomDetUnit*>(stackedGeometry->idToDetUnit(stDetId, 0));
  const GeomDet* det = reinterpret_cast<const GeomDet*>(stackedGeometry->idToDet(stDetId, 0));
  const PixelTopology& topo = unit->specificTopology();
  const Bounds& bounds = det->surface().bounds();

  std::pair<float, float> pitch = topo.pitch();
  stripPitch_ = pitch.first; // Strip pitch (or pixel pitch along shortest axis)
  stripLength_ = pitch.second;  //  Strip length (or pixel pitch along longest axis)
  nStrips_ = topo.nrows(); // No. of strips in sensor
  sensorWidth_ = bounds.width(); // Width of sensitive region of sensor (= stripPitch * nStrips).

  sigmaPerp_ = stripPitch_/sqrt(12.); // resolution perpendicular to strip (or to longest pixel axis)
  sigmaPar_  = stripLength_/sqrt(12.); // resolution parallel to strip (or to longest pixel axis)
}
