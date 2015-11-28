//
// <copyright file="LUSequenceReader.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// LUSequenceReader.cpp : Defines the exported functions for the DLL application.
//


#include "stdafx.h"
#include "Basics.h"
#define DATAREADER_EXPORTS  // creating the exports here
#include "DataReader.h"
#include "LUSequenceReader.h"
#ifdef LEAKDETECT
#include <vld.h> // leak detection
#endif
#include <fstream>
#include <random>       // std::default_random_engine
#include "fileutil.h"

namespace Microsoft { namespace MSR { namespace CNTK {

// GetIdFromLabel - get an Id from a Label
// mbStartSample - the starting sample we are ensureing are good
// endOfDataCheck - check if we are at the end of the dataset (no wraparound)
// returns - true if we have more to read, false if we hit the end of the dataset
template<class ElemType>
long LUSequenceReader<ElemType>::GetIdFromLabel(const LabelType& labelValue, LabelInfo& labelInfo)
{
    auto found = labelInfo.word4idx.find(labelValue);

    return found->second;
}

template<class ElemType>
BatchLUSequenceReader<ElemType>::~BatchLUSequenceReader()
{
    for (int index = labelInfoMin; index < labelInfoMax; ++index)
    {
        delete[] m_labelInfo[index].m_id2classLocal;
        delete[] m_labelInfo[index].m_classInfoLocal;
    };
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::ReadLabelInfo(const wstring & vocfile,
    map<wstring, long> & word4idx,
    bool readClass,
    map<wstring, long>& word4cls,
    map<long, wstring>& idx4word,
    map<long, long>& idx4class,
    int & mNbrCls)
{
    wifstream vin; 
#ifdef _MSC_VER
    vin.open(vocfile, wifstream::in);
#else
    vin.open(wtocharpath(vocfile), wifstream::in);
#endif
    if (!vin.good())
        LogicError("LUSequenceReader cannot open %ls\n", vocfile);

    wstring wstr = L" ";
    long b = 0;
    this->nwords = 0;
    int prevcls = -1;

    mNbrCls = 0;
    wstring strtmp;
    while (vin.good())
    {
        getline(vin, strtmp); 
        strtmp = wtrim(strtmp);
        if (strtmp.length() == 0)
            break; 
        if (readClass)
        {
            vector<wstring> wordandcls = wsep_string(strtmp, wstr);
            long cls = _wtoi(wordandcls[1].c_str());
            word4cls[wordandcls[0]] = cls;

            idx4class[b] = cls;

            if (idx4class[b] != prevcls)
            {
                if (idx4class[b] < prevcls)
                    LogicError("LUSequenceReader: the word list needs to be grouped into classes and the classes indices need to be ascending.");
                prevcls = idx4class[b];
            }

            word4idx[wordandcls[0]] = b;
            idx4word[b++] = wordandcls[0];
            if (mNbrCls < cls)
                mNbrCls = cls;
        }
        else
        {
            word4idx[strtmp] = b;
            idx4word[b++] = strtmp;
        }
        this->nwords++;
    }

    if (readClass)
        mNbrCls++;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::GetClassInfo(LabelInfo& lblInfo)
{
    if (lblInfo.m_clsinfoRead || lblInfo.mNbrClasses == 0) return;

    // populate local CPU matrix
    if (lblInfo.m_id2classLocal == nullptr)
        lblInfo.m_id2classLocal = new Matrix<ElemType>(CPUDEVICE);
    if (lblInfo.m_classInfoLocal == nullptr)
        lblInfo.m_classInfoLocal = new Matrix<ElemType>(CPUDEVICE);

    lblInfo.m_classInfoLocal->SwitchToMatrixType(MatrixType::DENSE, matrixFormatDense, false);
    lblInfo.m_classInfoLocal->Resize(2, lblInfo.mNbrClasses);
    lblInfo.m_classInfoLocal->SetValue(0);  // TODO: needed? (left-over of refactoring)

    //move to CPU since element-wise operation is expensive and can go wrong in GPU
    // TODO: Can it ever be not on the CPU? We allocate it ourselves abovew
    int curDevId = lblInfo.m_classInfoLocal->GetDeviceId();
    lblInfo.m_classInfoLocal->TransferFromDeviceToDevice(curDevId, CPUDEVICE, true, false, false);

    int clsidx;
    int prvcls = -1;
    for (size_t j = 0; j < this->nwords; j++)
    {
        clsidx = lblInfo.idx4class[(long)j];
        if (prvcls != clsidx)
        {
            if (prvcls >= 0)
                (*lblInfo.m_classInfoLocal)(1, prvcls) = (float)j;
            prvcls = clsidx;
            (*lblInfo.m_classInfoLocal)(0, prvcls) = (float)j;
        }
    }
    (*lblInfo.m_classInfoLocal)(1, prvcls) = (float)this->nwords;

    lblInfo.m_classInfoLocal->TransferFromDeviceToDevice(CPUDEVICE, curDevId, true, false, false);

    lblInfo.m_clsinfoRead = true;
}

// GetIdFromLabel - get an Id from a Label
// mbStartSample - the starting sample we are ensureing are good
// endOfDataCheck - check if we are at the end of the dataset (no wraparound)
// returns - true if we have more to read, false if we hit the end of the dataset
template<class ElemType>
bool LUSequenceReader<ElemType>::GetIdFromLabel(const vector<LabelIdType>& labelValue, vector<LabelIdType>& val)
{
    val.clear();

    for (size_t i = 0; i < labelValue.size(); i++)
    {
        val.push_back(labelValue[i]);
    }
    return true;    // TODO: what's this return value for?
}

template<class ElemType>
int LUSequenceReader<ElemType>::GetSentenceEndIdFromOutputLabel()
{
    LabelInfo& featIn = m_labelInfo[labelInfoOut];

    auto found = featIn.word4idx.find(featIn.endSequence);

    if (found != featIn.word4idx.end())
        return (int)found->second;
    else
        return -1;  // not found
}

// GetData - Gets metadata from the specified section (into CPU memory) 
// sectionName - section name to retrieve data from
// numRecords - number of records to read
// data - pointer to data buffer, if NULL, dataBufferSize will be set to size of required buffer to accomidate request
// dataBufferSize - [in] size of the databuffer in bytes
//                  [out] size of buffer filled with data
// recordStart - record to start reading from, defaults to zero (start of data)
// returns: true if data remains to be read, false if the end of data was reached
template<class ElemType>
bool LUSequenceReader<ElemType>::GetData(const std::wstring& , size_t , void* , size_t& , size_t )
{
    return false;
}

//bool LUSequenceReader<ElemType>::CheckIdFromLabel(const typename LUSequenceParser<ElemType>::LabelType& labelValue, const LabelInfo& labelInfo, typename LUSequenceParser<ElemType>::LabelIdType & labelId)
template<class ElemType>
bool LUSequenceReader<ElemType>::CheckIdFromLabel(const LabelType& labelValue, const LabelInfo& labelInfo, unsigned & labelId)
{
    auto found = labelInfo.mapLabelToId.find(labelValue);

    // not yet found, add to the map
    if (found == labelInfo.mapLabelToId.end())
    {
        return false; 
    }
    labelId = found->second;    // TODO: This function is called Check...() but it does Get something. Bad name?
    return true; 
}

template<class ElemType>
void LUSequenceReader<ElemType>::WriteLabelFile()
{
    // update the label dimension if it is not big enough, need it here because m_labelIdMax get's updated in the processing loop (after a read)
    for (int index = labelInfoMin; index < labelInfoMax; ++index)
    {
        LabelInfo& labelInfo = m_labelInfo[index];

        // write out the label file if they don't have one
        if (!labelInfo.fileToWrite.empty())
        {
            if (labelInfo.mapIdToLabel.size() > 0)
            {
                File labelFile(labelInfo.fileToWrite, fileOptionsWrite | fileOptionsText);
                for (int i=0; i < labelInfo.mapIdToLabel.size(); ++i)
                {
                    labelFile << labelInfo.mapIdToLabel[i] << '\n';
                }
                labelInfo.fileToWrite.clear();
            }
            else if (!m_cachingWriter)
            {
                fprintf(stderr, "WARNING: file %ls NOT written to disk, label files only written when starting at epoch zero!", labelInfo.fileToWrite.c_str());
            }
        }
    }
}

template<class ElemType>
void LUSequenceReader<ElemType>::LoadLabelFile(const std::wstring &filePath, std::vector<LabelType>& retLabels)
{
    // initialize with file name
    std::wstring path = filePath;
    
    retLabels.resize(0);
    wifstream vin;
#ifdef _MSC_VER
    vin.open(path.c_str(), ifstream::in);
#else
    vin.open(wtocharpath(path), ifstream::in);
#endif

    wstring str;
    while (vin.good())
    {
        wchar_t stmp[MAX_STRING];
        vin.getline(stmp, MAX_STRING);
        str = stmp;
        str = wtrim(str);
        if (str.length() == 0)
            break; 

        // check for a comment line
        wstring::size_type pos = str.find_first_not_of(L" \t");
        if (pos != -1)
        {
            str = wtrim(str);
            retLabels.push_back((LabelType)str);
        }
    }
}

template<class ElemType>
void LUSequenceReader<ElemType>::ChangeMaping(const map<LabelType, LabelType>& maplist,
    const LabelType & unkstr,
    map<LabelType, LabelIdType> & word4idx)
{
    auto punk = word4idx.find(unkstr);
    for(auto ptr = word4idx.begin(); ptr != word4idx.end(); ptr++)
    {
        LabelType wrd = ptr->first;
        LabelIdType idx = -1; 
        if (maplist.find(wrd) != maplist.end())
        {
            LabelType mpp = maplist.find(wrd)->second; 
            idx = word4idx[mpp];
        }
        else
        {
            if (punk == word4idx.end())
            {
                RuntimeError("check unk list is missing ");
            }
            idx = punk->second;
        }

        word4idx[wrd] = idx;
    }
}

template<class ElemType>
template<class ConfigRecordType>
void BatchLUSequenceReader<ElemType>::InitFromConfig(const ConfigRecordType & readerConfig)
{
    // See if the user wants caching
    m_cachingReader = NULL;
    m_cachingWriter = NULL;

    LoadWordMapping(readerConfig);

    std::vector<std::wstring> features;
    std::vector<std::wstring> labels;
    GetFileConfigNames(readerConfig, features, labels);
    if (features.size() > 0)
    {
        m_featuresName = features[0];
    }

    {
        wstring tInputLabel = readerConfig(L"inputLabel", L"");
        wstring tOutputLabel = readerConfig(L"outputLabel", L"");

        if (labels.size() == 2)
        {
            if (tInputLabel == L"" && tOutputLabel == L"")
            {
                for (int index = labelInfoMin; index < labelInfoMax; ++index)
                {
                    m_labelsName[index] = labels[index];
                }
            }
            else
            {
                int index = 0;
                for (int i = labelInfoMin; i < labelInfoMax; ++i)
                {
                    if (labels[i] == tInputLabel)
                        m_labelsName[index] = labels[i];
                }
                if (m_labelsName[index] == L"")
                    RuntimeError("cannot find input label");

                index = 1;
                for (int i = labelInfoMin; i < labelInfoMax; ++i)
                {
                    if (labels[i] == tOutputLabel)
                        m_labelsName[index] = labels[i];
                }
                if (m_labelsName[index] == L"")
                    RuntimeError("cannot find output label");
            }
        }
        else
            RuntimeError("two label definitions (in and out) required for Sequence Reader");

        //const ConfigRecordType & featureConfig = readerConfig(m_featuresName.c_str(), ConfigRecordType::Record());

        for (int index = labelInfoMin; index < labelInfoMax; ++index)
        {
            const ConfigRecordType & labelConfig = readerConfig(m_labelsName[index].c_str(), ConfigRecordType::Record());

            m_labelInfo[index].idMax = 0;
            m_labelInfo[index].beginSequence = (wstring)labelConfig(L"beginSequence", L"");
            m_labelInfo[index].endSequence   = (wstring)labelConfig(L"endSequence",   L"");
            m_labelInfo[index].busewordmap = labelConfig(L"useWordMap", false);

            m_labelInfo[index].isproposal = labelConfig(L"isProposal", false);

            m_labelInfo[index].m_clsinfoRead = false;

            // determine label type desired
            wstring labelType(labelConfig(L"labelType", L"category"));
            if (!_wcsicmp(labelType.c_str(), L"category"))
            {
                m_labelInfo[index].type = labelCategory;
            }
            else
                LogicError("LUSequence reader only supports category label");

            // if we have labels, we need a label Mapping file, it will be a file with one label per line
            if (m_labelInfo[index].type != labelNone)
            {
                wstring mode = labelConfig(L"mode", L"plain");//plain, class

                m_labelInfo[index].m_classInfoLocal = nullptr;
                m_labelInfo[index].m_id2classLocal = nullptr;

                if (mode == L"class")
                {
                    m_labelInfo[index].readerMode = ReaderMode::Class;
                }

                std::wstring wClassFile = labelConfig(L"token", L"");
                if (wClassFile != L"")
                {
                    ReadLabelInfo(wClassFile, m_labelInfo[index].word4idx, 
                        m_labelInfo[index].readerMode == ReaderMode::Class, 
                        m_labelInfo[index].word4cls,
                        m_labelInfo[index].idx4word, m_labelInfo[index].idx4class, m_labelInfo[index].mNbrClasses);

                    GetClassInfo(m_labelInfo[index]);
                }
                if (m_labelInfo[index].busewordmap)
                    ChangeMaping(mWordMapping, mUnkStr, m_labelInfo[index].word4idx);
                m_labelInfo[index].dim = (long)m_labelInfo[index].idx4word.size();
            }

        }
    }

    // initialize all the variables
    m_mbStartSample = m_epoch = m_totalSamples = m_epochStartSample = m_seqIndex = 0;
    m_endReached = false;
    m_readNextSampleLine = 0;
    m_readNextSample = 0;

    m_wordContext = readerConfig(L"wordContext", ConfigRecordType::Array(intargvector(vector<int>{ 0 })));

    // The input data is a combination of the label Data and extra feature dims together
//    m_featureCount = m_featureDim + m_labelInfo[labelInfoIn].dim;
    m_featureCount = 1; 

    std::wstring m_file = readerConfig(L"file");
    if (m_traceLevel > 0)
        fprintf(stderr, "reading sequence file %ls\n", m_file.c_str());

    const LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    const LabelInfo& labelOut = m_labelInfo[labelInfoOut];
    m_parser.ParseInit(m_file.c_str(), labelIn.dim, labelOut.dim, labelIn.beginSequence, labelIn.endSequence, labelOut.beginSequence, labelOut.endSequence, mUnkStr);

    mRequestedNumParallelSequences = readerConfig(L"nbruttsineachrecurrentiter", (size_t)1);

    mRandomize = false;
    if (readerConfig.Exists(L"randomize"))
    {
        string randomizeString = readerConfig(L"randomize");
        if (!_stricmp(randomizeString.c_str(), "none"))
        {
            ;
        }
        else if (!_stricmp(randomizeString.c_str(), "auto") || randomizeString == "True")
        {
            mRandomize = true;
        }
        // else invalid
    }

    mEqualLengthOutput = readerConfig(L"equalLength",   true);
    mAllowMultPassData = readerConfig(L"dataMultiPass", false);

    mIgnoreSentenceBeginTag = readerConfig(L"ignoresentencebegintag", false);
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::Reset()
{
    mProcessed.clear();
    mToProcess.clear();
    mLastProcessedSentenceId = 0;
    mPosInSentence = 0;
    mLastPosInSentence = 0;
    mNumRead = 0;

    m_labelTemp.clear();
    m_featureTemp.clear();
    m_parser.mSentenceIndex2SentenceInfo.clear();
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples)
{
    if (m_featuresBuffer==NULL)
    {
        const LabelInfo& labelInfo = m_labelInfo[( m_labelInfo[labelInfoOut].type == labelNextWord)?labelInfoIn:labelInfoOut];
        m_featuresBuffer = new ElemType[mbSize*labelInfo.dim]();
        //memset(m_featuresBuffer,0,sizeof(ElemType)*mbSize*labelInfo.dim);
    }

    if (m_labelsBuffer==NULL)
    {
        const LabelInfo& labelInfo = m_labelInfo[( m_labelInfo[labelInfoOut].type == labelNextWord)?labelInfoIn:labelInfoOut];
        if (labelInfo.type == labelCategory)
        {
            m_labelsBuffer = new ElemType[labelInfo.dim*mbSize]();
            //memset(m_labelsBuffer,0,sizeof(ElemType)*labelInfo.dim*mbSize);
            m_labelsIdBuffer = new long[mbSize]();
            //memset(m_labelsIdBuffer,0,sizeof(long)*mbSize);
        }
        else if (labelInfo.type != labelNone)
        {
            m_labelsBuffer = new ElemType[mbSize]();
            //memset(m_labelsBuffer,0,sizeof(ElemType)*mbSize);
            m_labelsIdBuffer = NULL;
        }
    }

    m_mbSize = mbSize;
    m_epochSize = requestedEpochSamples;

    // we use epochSize, which might not be set yet, so use a default value for allocations if not yet set
    m_epoch = epoch;
    m_mbStartSample = epoch*m_epochSize;

    // allocate room for the data
    m_featureData.reserve(m_featureCount*m_mbSize);
    if (m_labelInfo[labelInfoOut].type == labelCategory)
        m_labelIdData.reserve(m_mbSize);
    else if (m_labelInfo[labelInfoOut].type != labelNone)
        m_labelData.reserve(m_mbSize);
    m_sequence.reserve(m_seqIndex); // clear out the sequence array

    m_clsinfoRead = false; 
    m_idx2clsRead = false; 

    mTotalSentenceSofar = 0;
    m_totalSamples = 0;

    Reset();

    m_parser.ParseReset(); /// restart from the corpus beginning
}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::FindNextSentences(size_t numRead)
{
    // I think this may test whether all sentences in a series of minibatches have hit their end. Then we'd pick the next sentences.
    if (mToProcess.size() > 0 && mProcessed.size() > 0)
    {
        bool allDone = true; 
        for (int s = 0; s < mToProcess.size(); s++)
        {
            size_t seq = mToProcess[s];
            if (mProcessed[seq] == false)
            {
                allDone = false;
                break;
            }
        }
        if (allDone)
        {
            mLastPosInSentence = 0;     // reset BPTT offset
            mToProcess.clear();         // means we need to get a new set of sentences and start over with them
            // reset sentence begin and end
            mSentenceEnd = false;
            mSentenceBegin = false;
        }
    }

    if (mToProcess.size() > 0 && mProcessed.size() > 0)
    {
        // I think if we get here then we are continuing to return the next sub-stretch of the same sentences in mToProcess[]
        size_t nbrToProcess = mToProcess.size();
        mSentenceBeginAt.resize(nbrToProcess, -1);      // if the start or end fall within the current sub-stretch, then it will be put in here
        mSentenceEndAt.resize(nbrToProcess, -1);
        mSentenceLengths.clear();
        mMaxSentenceLength = 0;

        for (size_t i = 0; i < nbrToProcess; i++)
        {
            size_t seq = mToProcess[i];
            size_t len = m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
            mSentenceLengths.push_back(len);
            mMaxSentenceLength = max(mMaxSentenceLength, len); 
        }
        return mToProcess.size();
    }

    mMaxSentenceLength = 0;

    // I think we get here if we need to start with the next batch of sentences
    if (m_parser.mSentenceIndex2SentenceInfo.size() == 0)       // corpus empty??
        return 0;

    // form mToProcess[] array for this minibatch
    vector<size_t> sln;     // (value of mSentenceLengths is first formed here and later moved over)
    size_t iNumber = min(numRead, mProcessed.size());
    int previousLn = -1;
    for (size_t seq = mLastProcessedSentenceId, inbrReader = 0; seq < mProcessed.size() && inbrReader < iNumber; seq++)
    {
        if (mProcessed[seq])
            continue;

        if (mEqualLengthOutput)
        {
            if (mProcessed[seq] == false && mToProcess.size() < mRequestedNumParallelSequences)
            {
                int ln = (int)m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
                if (ln == previousLn || previousLn == -1)
                {
                    sln.push_back(ln);
                    mToProcess.push_back(seq);
                    mMaxSentenceLength = max((int)mMaxSentenceLength, ln);
                    if (previousLn == -1)
                        mLastProcessedSentenceId = seq + 1;  /// update index for the next retrieval
                    previousLn = ln;
                }
            }

            if (mToProcess.size() == mRequestedNumParallelSequences)
                break;
            inbrReader++;
        }
        else
        {
            if (mProcessed[seq] == false && mToProcess.size() < mRequestedNumParallelSequences)
            {
                size_t len = m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
                sln.push_back(len);
                mToProcess.push_back(seq);
                mMaxSentenceLength = max(mMaxSentenceLength, len);
            }

            if (mToProcess.size() == mRequestedNumParallelSequences)
                break;
            inbrReader++;
        }
    }

    size_t nbrToProcess = mToProcess.size();
    mSentenceBeginAt.resize(nbrToProcess, -1);
    mSentenceEndAt.resize(nbrToProcess, -1);

    mSentenceLengths = sln;

    return mToProcess.size();
}

// fetch the next minibatch
template<class ElemType>
bool BatchLUSequenceReader<ElemType>::EnsureDataAvailable(size_t /*mbStartSample*/)
{
    m_featureData.clear();
    m_labelIdData.clear();
    m_featureWordContext.clear();

    // now get the labels
    LabelInfo& featIn  = m_labelInfo[labelInfoIn];
    LabelInfo& labelIn = m_labelInfo[labelInfoOut];

    // see how many we already read
    std::vector<SequencePosition> seqPos;

    if (mTotalSentenceSofar > m_epochSize)
    {
        m_pMBLayout->Init(0, 0);
        return false;
    }
    else
    {
        size_t nbrSentenceRead = FindNextSentences(mRequestedNumParallelSequences);
        if (mAllowMultPassData && nbrSentenceRead == 0 && mTotalSentenceSofar > 0 && m_totalSamples < m_epochSize)
        {
            // restart for the next pass of the data
            mProcessed.assign(mProcessed.size(), false);
            mLastProcessedSentenceId = 0;
            nbrSentenceRead = FindNextSentences(mRequestedNumParallelSequences);
        }

        if (nbrSentenceRead == 0)
        {
            Reset();

            mNumRead = m_parser.Parse(CACHE_BLOG_SIZE, &m_labelTemp, &m_featureTemp, &seqPos, featIn.word4idx, labelIn.word4idx, mAllowMultPassData);
            if (mNumRead == 0)
            {
                fprintf(stderr, "EnsureDataAvailable: No more data.\n");
                m_pMBLayout->Init(0, 0);
                return false;
            }
            mProcessed.assign(mNumRead, false);

#ifndef DEBUG_READER
            if (mRandomize)
            {
                unsigned seed = this->m_seed; 
                std::shuffle(m_parser.mSentenceIndex2SentenceInfo.begin(), m_parser.mSentenceIndex2SentenceInfo.end(), std::default_random_engine(seed));
                this->m_seed++;
            }
#endif

            m_readNextSampleLine += mNumRead;
            nbrSentenceRead = FindNextSentences(mRequestedNumParallelSequences);
            if (nbrSentenceRead == 0)
            {
                m_pMBLayout->Init(0, 0);
                return false;
            }
        }

        mTotalSentenceSofar += (ULONG) nbrSentenceRead;

        if (mLastPosInSentence != 0)
            RuntimeError("LUSequenceReader : only support beginning sentence at zero");
        if (mSentenceBeginAt.size() != mToProcess.size())
            RuntimeError("LUSequenceReader : need to preallocate mSentenceBegin");
        if (mSentenceEndAt.size() != mToProcess.size())
            RuntimeError("LUSequenceReader : need to preallocate mSentenceEnd");
        if (mMaxSentenceLength > m_mbSize)
            RuntimeError("LUSequenceReader : minibatch size needs to be large enough to accomodate the longest sentence");

        // reset all sentence-end indices to NO_INPUT, which is negative
        mSentenceEndAt.assign(mSentenceEndAt.size(), NO_INPUT);

        // add one minibatch 
        int i;
        int j = 0;
        m_pMBLayout->Init(mToProcess.size(), mMaxSentenceLength);
        if (mLastPosInSentence != 0)
            LogicError("LUBatchSequenceReader: Currently, mLastPosInSentence != 0 is not supported.");
        for (i = (int)mLastPosInSentence; j < (int)mMaxSentenceLength; i++, j++)
        {
            assert(i == j); // for now
            for (int k = 0; k < mToProcess.size(); k++)
            {
                size_t seq = mToProcess[k];         // sequence index
                size_t seqLen = m_parser.mSentenceIndex2SentenceInfo[seq].sLen;

                if (i == mLastPosInSentence)        // first token in the sequence
                {
                    mSentenceBeginAt[k] = i;
                    if (!mIgnoreSentenceBeginTag)   // ignore sentence begin, this is used for decoder network reader, which carries activities from the encoder networks
                        m_pMBLayout->Set(k, j, MinibatchPackingFlags::SequenceStart);
                }

                if (i == seqLen - 1)    // last token in the sequence
                {
                    mSentenceEndAt[k] = i;
                    m_pMBLayout->Set(k, j, MinibatchPackingFlags::SequenceEnd);
                }

                if (i < seqLen)         // valid token
                {
                    size_t label = m_parser.mSentenceIndex2SentenceInfo[seq].sBegin + i;
                    std::vector<std::vector<LabelIdType>> tmpCxt;

                    for (int i_cxt = 0; i_cxt < m_wordContext.size(); i_cxt++)
                    {
                        if (featIn.type == labelCategory)
                        {
                            vector<LabelIdType> index;
                            int ilabel = (int) label + m_wordContext[i_cxt];
                            if (ilabel < m_parser.mSentenceIndex2SentenceInfo[seq].sBegin)
                            {
                                GetIdFromLabel(m_featureTemp[m_parser.mSentenceIndex2SentenceInfo[seq].sBegin], index);
                            }
                            else if (ilabel >= m_parser.mSentenceIndex2SentenceInfo[seq].sEnd)
                            {
                                GetIdFromLabel(m_featureTemp[m_parser.mSentenceIndex2SentenceInfo[seq].sEnd - 1], index);
                            }
                            else
                            {
                                GetIdFromLabel(m_featureTemp[ilabel], index);
                            }
                            if (i_cxt == 0)
                            {
                                m_featureData.push_back(index);
                            }
                            tmpCxt.push_back(index);
                        }
                        else
                        {
                            RuntimeError("Input label expected to be a category label");
                        }
                    }

                    m_featureWordContext.push_back(tmpCxt);

                    // now get the output label
                    LabelIdType id = m_labelTemp[label];
                    m_labelIdData.push_back(id);

                    m_totalSamples++;
                }
                else            // i >= seqLen: no token for this sequence (NoInput)
                {
                    // push null 
                    std::vector<std::vector<LabelIdType>> tmpCxt;
                    std::vector<LabelIdType> index;
                    for (int i_cxt = 0; i_cxt < m_wordContext.size(); i_cxt++)
                        index.push_back((LabelIdType)NULLLABEL);
                    tmpCxt.push_back(index);
                    m_featureWordContext.push_back(tmpCxt);

                    m_labelIdData.push_back((LabelIdType)NULLLABEL);
                    m_pMBLayout->Set(k, j, MinibatchPackingFlags::NoInput);
                }

            }
        }

        mLastPosInSentence = (i == mMaxSentenceLength)?0:i;
    }

    return true;
}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::GetNumParallelSequences()
{
    size_t sz = (mSentenceBeginAt.size() == 0) ? mRequestedNumParallelSequences/*not initialized yet?*/ : mSentenceBeginAt.size();
    if (mSentenceBeginAt.size() == 0)
        mSentenceBeginAt.assign(sz, -1);
    if (mSentenceEndAt.size() == 0)
        mSentenceEndAt.assign(sz, -1);
    return sz;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetNumParallelSequences(const size_t mz)
{
    mRequestedNumParallelSequences = mz;
}

template<class ElemType>
bool BatchLUSequenceReader<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{

    // get out if they didn't call StartMinibatchLoop() first
    if (m_mbSize == 0)
    {
        fprintf(stderr, "GetMiniBatch : m_mbSize = 0\n");
        return false;
    }

    bool moreData = EnsureDataAvailable(m_mbStartSample);
    if (moreData == false)
        return false;

    // actual size is the size of the next seqence
    size_t actualmbsize = 0;
    size_t lablsize = 0;

    // figure out the size of the next sequence
    actualmbsize = m_labelIdData.size();
    if (actualmbsize > m_mbSize * mToProcess.size())
        RuntimeError("Specified minibatch size %d is smaller than the actual minibatch size %d.", (int)m_mbSize, (int)actualmbsize);

    // now get the labels
    const LabelInfo& featInfo = m_labelInfo[labelInfoIn];

    if (actualmbsize > 0)
    {

        //loop through all the samples
        Matrix<ElemType>& features = *matrices[m_featuresName];
        Matrix<ElemType>  locObs(CPUDEVICE);
        if (features.GetMatrixType() == DENSE)
            locObs.SwitchToMatrixType(DENSE, features.GetFormat(), false);
        else
            locObs.SwitchToMatrixType(SPARSE, matrixFormatSparseCSC, false);

        if (matrices.find(m_featuresName) == matrices.end())
        {
            RuntimeError("LUsequence reader cannot find %ls.", m_featuresName.c_str());
        }

        locObs.Resize(featInfo.dim * m_wordContext.size(), actualmbsize);
        locObs.SetValue(0);

        size_t utt_id = 0;
        for (size_t j = 0; j < actualmbsize; ++j)
        {
            utt_id = (size_t) fmod(j, mSentenceEndAt.size());  /// get the utterance id

            size_t utt_t = (size_t) floor(j/mSentenceEndAt.size()); /// the utt-specific timing

            // vector of feature data goes into matrix column
            for (size_t jj = 0; jj < m_featureWordContext[j].size(); jj++) ///  number of sentence per time
            {
                /// this support context dependent inputs since words or vector of words are placed
                /// in different slots
                for (size_t ii = 0; ii < m_featureWordContext[j][jj].size(); ii++)  /// context
                {
                    /// this can support bag of words, since words are placed in the same slot
                    size_t idx = m_featureWordContext[j][jj][ii];

                    if (idx >= featInfo.dim)
                    {
                        if (m_pMBLayout->Is(utt_id, utt_t, MinibatchPackingFlags::NoInput)) /// for those obs that are for no observations
                            LogicError("BatchLUSequenceReader::GetMinibatch observation is larger than its dimension but no_labels sign is not used to indicate that this observation has no labels. Possible reason is a bug in EnsureDataAvailable or a bug here. ");
                        continue;
                    }

                    assert(idx < featInfo.dim);
                    if (utt_t > mSentenceEndAt[utt_id]) 
                        locObs.SetValue(idx + jj * featInfo.dim, j, (ElemType)0);
                    else
                        locObs.SetValue(idx + jj * featInfo.dim, j, (ElemType)1);
                }
            }
        }

        features.SetValue(locObs);
        
        lablsize = GetLabelOutput(matrices, m_labelInfo[labelInfoOut], actualmbsize);

        // go to the next sequence
        m_seqIndex++;
    }
    else
    {
        fprintf(stderr, "actual minibatch size is zero\n");
        return 0;
    }

    // we read some records, so process them
    if (actualmbsize == 0)
        return false;
    else
        return true;
}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::GetLabelOutput(std::map<std::wstring, 
    Matrix<ElemType>*>& matrices, LabelInfo& labelInfo, size_t actualmbsize)
{
    Matrix<ElemType>* labels = matrices[m_labelsName[labelInfoOut]];
    if (labels == nullptr) return 0;

    DEVICEID_TYPE device = labels->GetDeviceId();

    labels->Resize(labelInfo.dim, actualmbsize);
    labels->SetValue(0);
    labels->TransferFromDeviceToDevice(device, CPUDEVICE, true);

    size_t nbrLabl = 0;
    for (size_t j = 0; j < actualmbsize; ++j)
    {
        long wrd = m_labelIdData[j];

        size_t utt_id = (size_t) fmod(j, mSentenceBeginAt.size());
        size_t utt_t = (size_t) floor(j / mSentenceBeginAt.size());

        if (utt_t > mSentenceEndAt[utt_id]) continue;
        if (labelInfo.readerMode == ReaderMode::Plain)
            labels->SetValue(wrd, j, 1); 
        else if (labelInfo.readerMode == ReaderMode::Class && labelInfo.mNbrClasses > 0)
        {
            labels->SetValue(0, j, (ElemType)wrd);

            long clsidx = -1;
            clsidx = labelInfo.idx4class[wrd];

            labels->SetValue(1, j, (ElemType)clsidx);
            /// save the [beginning ending_indx) of the class 
            ElemType lft = (*labelInfo.m_classInfoLocal)(0, clsidx);
            ElemType rgt = (*labelInfo.m_classInfoLocal)(1, clsidx);
            if (rgt <= lft)
                LogicError("LUSequenceReader : right is equal or smaller than the left, which is wrong.");
            labels->SetValue(2, j, lft); /// beginning index of the class
            labels->SetValue(3, j, rgt); /// end index of the class
        }
        else
            LogicError("LUSequenceReader: reader mode is not set to Plain. Or in the case of setting it to Class, the class number is 0. ");
        nbrLabl++;
    }

    return nbrLabl;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::CopyMBLayoutTo(MBLayoutPtr pMBLayout)
{
    pMBLayout->CopyFrom(m_pMBLayout);
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetSentenceEnd(int wrd, int pos, int actualMbSize)
{
    // now get the labels
    LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    LabelIdType index = GetIdFromLabel(labelIn.endSequence.c_str(), labelIn);

    if (pos == actualMbSize - 1) 
    {
        if (wrd == (int)index)
            mSentenceEnd = true;
        else
            mSentenceEnd = false; 
    }
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetSentenceBegin(int wrd, int pos, int /*actualMbSize*/)
{
    // now get the labels
    LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    LabelIdType index = GetIdFromLabel(labelIn.beginSequence.c_str(), labelIn);

    if (pos == 0) 
    {
        if (wrd == (int)index)
            mSentenceBegin = true;
        else
            mSentenceBegin = false; 
    }
}


template<class ElemType>
bool BatchLUSequenceReader<ElemType>::DataEnd(EndDataType endDataType)
{
    bool ret = false;
    switch (endDataType)
    {
    case endDataNull:
        assert(false);
        break;
    case endDataEpoch:
    case endDataSet:
        ret = !EnsureDataAvailable(m_mbStartSample);
        break;
    case endDataSentence:  // for fast reader each minibatch is considered a "sentence", so always true
        if (mSentenceEndAt.size() != mToProcess.size())
            LogicError("DataEnd: Sentence ending vector size %d and the toprocess vector size %d should be the same.", (int)mSentenceEndAt.size(), (int)mToProcess.size());
        ret = true;
        for (size_t i = 0; i < mToProcess.size(); i++)
        {
            if (mSentenceEndAt[i] == NO_INPUT)
                LogicError("BatchLUSequenceReader: Minibatch should be large enough to accomodate the longest sentence.");
            size_t k = mToProcess[i];
            mProcessed[k] = true;
        }
        break;
    }
    return ret;

}

template<class ElemType>
bool BatchLUSequenceReader<ElemType>::CanReadFor(wstring nodeName)  // TODO: const wstring &
{
    if (this->m_featuresName == nodeName) return true;
    else if (m_labelsName[labelInfoIn] == nodeName) return true;
    else if (m_labelsName[labelInfoOut] == nodeName) return true;
    else return false;
}

/// get a column slice corresponding to a frame of observations
template<class ElemType>
bool BatchLUSequenceReader<ElemType>::GetFrame(std::map<std::wstring, Matrix<ElemType>*>& matrices, const size_t tidx, vector<size_t>& history)
{

    // get out if they didn't call StartMinibatchLoop() first
    if (m_mbSize == 0)
        return false;

    LabelInfo& labelIn = m_labelInfo[labelInfoIn];

    if (m_labelInfo[labelInfoIn].isproposal)
    {
        const LabelInfo& featInfo = m_labelInfo[labelInfoIn];

        //loop through all the samples
        Matrix<ElemType>& features = *matrices[m_featuresName];
        Matrix<ElemType>  locObs(CPUDEVICE);
        locObs.SwitchToMatrixType(SPARSE, matrixFormatSparseCSC, false);

        if (matrices.find(m_featuresName) == matrices.end())
        {
            RuntimeError("LUSequenceReader cannot find %ls", m_featuresName.c_str());
        }
        locObs.Resize(featInfo.dim * m_wordContext.size(), mRequestedNumParallelSequences);
        locObs.SetValue(0);

        assert(mRequestedNumParallelSequences == 1);    // currently only support one utterance a time

        size_t hlength = history.size();
        int nextProposal = -1;
        if (hlength == 0)
        {
            LabelIdType index;

            index = GetIdFromLabel(m_labelInfo[labelInfoIn].beginSequence.c_str(), labelIn);

            nextProposal = index;
            history.push_back(nextProposal);
        }

        for (size_t j = 0; j < mRequestedNumParallelSequences; ++j)
        {
            for (size_t jj = 0; jj < m_wordContext.size(); jj++)
            {
                int cxt = m_wordContext[jj];

                /// assert that wordContext is organized as descending order
                assert((jj == m_wordContext.size() - 1) ? true : cxt > m_wordContext[jj + 1]);

                size_t hidx;
                size_t hlength = history.size();
                if (hlength + cxt > 0)
                    hidx = history[hlength + cxt - 1];
                else
                    hidx = history[0];

                if (matrices.find(m_featuresName) != matrices.end())
                {
                    locObs.SetValue(hidx + jj * featInfo.dim, j, (ElemType)1);
                }
            }
        }

        features.SetValue(locObs);
    }
    else {
        for (typename map<wstring, Matrix<ElemType>>::iterator p = mMatrices.begin(); p != mMatrices.end(); p++)
        {
            assert(mMatrices[p->first].GetNumCols() > tidx);
            if (matrices.find(p->first) != matrices.end())
                matrices[p->first]->SetValue(mMatrices[p->first].ColumnSlice(tidx, mRequestedNumParallelSequences));
        }
    }

    // we read some records, so process them
    return true;
}

/// propose labels, return a vector with size larger than 0 if this reader allows proposal
/// otherwise, return a vector with length zero
template<class ElemType>
void BatchLUSequenceReader<ElemType>::InitProposals(map<wstring, Matrix<ElemType>*>& pMat)
{
    if (m_labelInfo[labelInfoIn].isproposal)
    {
        /// no need to save info for labelInfoIn since it is in mProposals
        if (pMat.find(m_labelsName[labelInfoOut]) != pMat.end())
            mMatrices[m_labelsName[labelInfoOut]].SetValue(*(pMat[m_labelsName[labelInfoOut]]));
    }
    else {
        if (pMat.find(m_featuresName) != pMat.end())
            mMatrices[m_featuresName].SetValue(*(pMat[m_featuresName]));
    }
}

template<class ElemType>
template<class ConfigRecordType>
void BatchLUSequenceReader<ElemType>::LoadWordMapping(const ConfigRecordType& readerConfig)
{
    mWordMappingFn = (wstring)readerConfig(L"wordmap", L"");
    wstring si, so;
    wstring ss;
    vector<wstring> vs;
    if (mWordMappingFn != L"")
    {
        wifstream fp;
#ifdef _WIN32
        fp.open(mWordMappingFn.c_str(), wifstream::in);
#else
        fp.open(wtocharpath(mWordMappingFn.c_str()).c_str(), wifstream::in);
#endif

        while (fp.good())
        {
            getline(fp, ss);
            ss = wtrim(ss);
            if (ss.length() == 0)
                break; 
            vs = wsep_string(ss, L" ");
            si = vs[0]; so = vs[1];
            mWordMapping[si] = so;
        }
        fp.close();
    }
    mUnkStr = (wstring)readerConfig(L"unk", L"<unk>");
}

template class BatchLUSequenceReader<double>;
template class BatchLUSequenceReader<float>;

template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{
    /// on first iteration, need to check if all requested data matrices are available
    std::map<std::wstring, size_t>::iterator iter;
    if (mCheckDictionaryKeys)
    {
        for (auto iter = matrices.begin(); iter != matrices.end(); iter++)
        {
            bool bFound = false;
            for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
            {
                if ((p->second)->CanReadFor(iter->first))
                {
                    nameToReader[iter->first] = p->second;
                    bFound = true;

                    break;
                }
            }
            if (bFound == false)
                RuntimeError("GetMinibatch: cannot find a node that can feed in features for %ls", iter->first.c_str());
        }
        mCheckDictionaryKeys = false;
    }

    /// set the same random seed
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        p->second->SetRandomSeed(this->m_seed);
    }
    this->m_seed++;

    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        if ((p->second)->GetMinibatch(matrices) == false)
            return false;
    }

    return true;
}

/// set the same random seed
template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::SetRandomSeed(int us)
{
    this->m_seed = us;
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        p->second->SetRandomSeed(this->m_seed);
    }
}

template<class ElemType>
template<class ConfigRecordType>
void MultiIOBatchLUSequenceReader<ElemType>::InitFromConfig(const ConfigRecordType & readerConfig)
{
    vector<wstring> ioNames = readerConfig(L"ioNodeNames", ConfigRecordType::Array(stringargvector()));
    if (ioNames.size() > 0)
    {
        /// newer code that explicitly place multiple streams for inputs
        foreach_index(i, ioNames) // inputNames should map to node names
        {
            const ConfigRecordType & thisIO = readerConfig(ioNames[i]);

            BatchLUSequenceReader<ElemType> *thisReader = new BatchLUSequenceReader<ElemType>();
            thisReader->Init(thisIO);

            pair<wstring, BatchLUSequenceReader<ElemType>*> pp(ioNames[i], thisReader);

            mReader.insert(pp);
        }
    }
    else{
        /// older code that assumes only one stream of feature
        BatchLUSequenceReader<ElemType> *thisReader = new BatchLUSequenceReader<ElemType>();

        thisReader->Init(readerConfig);

        pair<wstring, BatchLUSequenceReader<ElemType>*> pp(msra::strfun::wstrprintf(L"stream%d", mReader.size()), thisReader);

        mReader.insert(pp);
    }
}

template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        (p->second)->StartMinibatchLoop(mbSize, epoch, requestedEpochSamples);
    }
}

template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::CopyMBLayoutTo(MBLayoutPtr pMBLayout)
{
    /// run for each reader
    vector<size_t> col;
    size_t rows = 0, cols = 0;
    for (const auto & p : mReader)
    {
        p.second->CopyMBLayoutTo(pMBLayout);
        if (rows == 0)
            rows = pMBLayout->GetNumParallelSequences();
        else if (rows != pMBLayout->GetNumParallelSequences())
            LogicError("multiple streams for LU sequence reader must have the same number of rows for sentence beginning");
        size_t this_col = pMBLayout->GetNumTimeSteps();
        col.push_back(this_col);
        cols += this_col;
    }
}

template<class ElemType>
size_t MultiIOBatchLUSequenceReader<ElemType>::GetNumParallelSequences()
{
    return mReader.begin()->second->GetNumParallelSequences();
}

template<class ElemType>
int MultiIOBatchLUSequenceReader<ElemType>::GetSentenceEndIdFromOutputLabel()
{
    if (mReader.size() != 1)
        LogicError("GetSentenceEndIdFromOutputLabel: support only for one reader in MultiIOBatchLUSequenceReader");
    int iret = -1;

    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        iret = (p->second)->GetSentenceEndIdFromOutputLabel();
    }
    return iret;
}

template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::DataEnd(EndDataType endDataType)
{
    bool ret = true;
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        ret |= (p->second)->DataEnd(endDataType);
    }
    return ret;
}

/// history is shared
template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::GetProposalObs(std::map<std::wstring, Matrix<ElemType>*>& matrices, const size_t tidx, vector<size_t>& history)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        if ((p->second)->GetFrame(matrices, tidx, history) == false)
        {
            return false;
        }
    }
    return true;
}

/// need to provide initial matrice values if there are
/// these values are from getMinibatch
template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::InitProposals(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        (p->second)->InitProposals(matrices);
    }
}

template class MultiIOBatchLUSequenceReader<double>;
template class MultiIOBatchLUSequenceReader<float>;


}}}
