package edu.stanford.nlp.naturalli.entail;

import edu.stanford.nlp.classify.*;
import edu.stanford.nlp.io.IOUtils;
import edu.stanford.nlp.optimization.OWLQNMinimizer;
import edu.stanford.nlp.pipeline.StanfordCoreNLP;
import edu.stanford.nlp.util.*;
import edu.stanford.nlp.util.logging.RedwoodConfiguration;

import java.io.*;
import java.util.*;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Stream;
import java.util.zip.GZIPInputStream;

import static edu.stanford.nlp.util.logging.Redwood.Util.*;

/**
 * Train an entailment classifier, constrained to various degrees by natural logic.
 *
 * @author Gabor Angeli
 */
public class ClassifierTrainer {

  enum ClassifierType{ SIMPLE, DISTANT }
  @Execution.Option(name="classifier", gloss="The type of classifier to train")
  public ClassifierType CLASSIFIER = ClassifierType.SIMPLE;

  @Execution.Option(name="train.file", gloss="The file to use for training the classifier")
  public File TRAIN_FILE = new File("etc/aristo/turk_90_trainset.tab");
  @Execution.Option(name="train.cache", gloss="A cache of the training annotations")
  public File TRAIN_CACHE = null;
  @Execution.Option(name="train.cache.do", gloss="If false, do not cache the training annotations")
  public boolean TRAIN_CACHE_DO = true;
  @Execution.Option(name="train.count", gloss="The number of training examples to use.")
  public int TRAIN_COUNT = -1;
  @Execution.Option(name="train.debugdoc", gloss="A LaTeX document with debugging information about training")
  public File TRAIN_DEBUGDOC = new File("tmp/train_debug.tex");

  @Execution.Option(name="train.distsup.iters", gloss="The number of iterations to run EM for")
  public int TRAIN_DISTSUP_ITERS = 10;
  @Execution.Option(name="train.distsup.sample_count", gloss="The number of iterations to run EM for")
  public int TRAIN_DISTSUP_SAMPLE_COUNT = 1000;

  @Execution.Option(name="train.feature_count_threshold", gloss="The minimum number of times we have to see a feature before considering it.")
  public int TRAIN_FEATURE_COUNT_THRESHOLD = 0;
  private static enum Regularizer {L1, L2}
  @Execution.Option(name="train.regularizer", gloss="The type of regularization to use (e.g., L1, L2)")
  public Regularizer TRAIN_REGULARIZER = Regularizer.L2;
  @Execution.Option(name="train.sigma", gloss="The regularization constant sigma for the classifier")
  public double TRAIN_SIGMA = 1.00;

  @Execution.Option(name="testFile", gloss="The file to use for testing the classifier")
  public File TEST_FILE = TRAIN_FILE;
  @Execution.Option(name="testCache", gloss="A cache of the test annotations")
  public File TEST_CACHE = null;
  @Execution.Option(name="test.cache.do", gloss="If false, do not cache the test annotations")
  public boolean TEST_CACHE_DO = true;

  @Execution.Option(name="model", gloss="The file to load/save the model to/from.")
  public static File MODEL = new File("tmp/model.ser.gz");
  @Execution.Option(name="retrain", gloss="If true, force the retraining of the classifier")
  public boolean RETRAIN = true;

  @Execution.Option(name="parallel", gloss="If true, run the featurizer in parallel")
  public boolean PARALLEL = false;

  public final EntailmentFeaturizer featurizer;

  public ClassifierTrainer(EntailmentFeaturizer featurizer) {
    this.featurizer = featurizer;
  }


  /**
   * Read a dataset from a given source, backing off to a given (potentially null or empty) cache.
   *
   * @param source The source file to read the dataset from.
   * @param cache The cache file to use if it exists and is non-empty.
   * @param size The number of datums to read.
   *
   * @return A stream of datums; note that this is lazily loaded, and may be null near the end (e.g., for parallel computations).
   *
   * @throws IOException Throw by the underlying read mechanisms.
   */
  public <E> Stream<E> readDataset(File source, File cache, int size,
                                   Function<Quintuple<Trilean, String, String, Optional, Optional>, E> datumFactory,
                                   Function<InputStream, Stream<E>> deserialize) throws IOException {
    // Read size
    if (size < 0) {
      size = 0;
      forceTrack("Counting lines");
      BufferedReader scanner = IOUtils.readerFromFile(source);
      while (scanner.readLine() != null) {
        size += 1;
      }
      log("dataset has size " + size);
      endTrack("Counting lines");
    }

    // Create stream
    if (cache != null && cache.exists()) {
      log("reading from cache (" + cache + ")");
      return deserialize.apply(new GZIPInputStream(new BufferedInputStream(new FileInputStream(cache)))).limit(size);
    } else {
      log("no cached version found. Reading from file (" + source + ")");
      BufferedReader reader = IOUtils.readerFromFile(source);
      return Stream.generate(() -> {
        try {
          String line;
          synchronized (reader) {  // In case we're reading multithreaded
            line = reader.readLine();
          }
          if (line == null) {
            return null;
          }
          // Parse the line
          String[] fields = line.split("\t");
          Trilean truth = Trilean.fromString(fields[1].toLowerCase());
          String premise = fields[2];
          String conclusion = fields[3];
          Optional<String> focus = Optional.empty();
          if (fields.length > 4 && !"".equals(fields[4])) {
            focus = Optional.of(fields[4]);
          }
          Optional<Double> luceneScore = Optional.empty();
          if (fields.length > 5 && !"".equals(fields[5])) {
            luceneScore = Optional.of(Double.parseDouble(fields[5]));
          }
          // Add the pair
          return datumFactory.apply(Quintuple.makeQuadruple(truth, premise, conclusion, focus, luceneScore));
        } catch (IOException e) {
          throw new RuntimeException(e);
        }
      }).limit(size);
    }
  }

  public Stream<EntailmentPair> readFlatDataset(File source, File cache, int size) throws IOException {
    return readDataset(source, cache, size,
        args -> new EntailmentPair(args.first, args.second, args.third, args.fourth, args.fifth),
        EntailmentPair::deserialize);
  }

  public Stream<DistantEntailmentPair> readDistantDataset(File source, File cache, int size, StanfordCoreNLP pipeline) throws IOException {
    return readDataset(source, cache, size,
        args -> new DistantEntailmentPair(args.first, args.second, args.third, args.fourth, args.fifth, pipeline),
        DistantEntailmentPair::deserialize);
  }

  /**
   * Wrap the training script for a classifier around some code to load it if possible.
   *
   * @param supplier The code to train a classifier, if it cannot be loaded
   * @param <E> The type of classifier we are training
   *
   * @return An entailment classifier of the given type, either loaded or trained as appropriate.
   */
  @SuppressWarnings("unchecked")
  public <E extends EntailmentClassifier> E trainOrLoad(Supplier<E> supplier) {
    // (save classifier)
    if (MODEL != null) {
      if (!RETRAIN && MODEL.exists() && MODEL.canRead()) {
        return (E) EntailmentClassifier.loadNoErrors(MODEL);
      } else {
        E classifier = supplier.get();
        try {
          classifier.save(MODEL);
          log("saved classifier to " + MODEL);
        } catch (Throwable t) {
          err("ERROR SAVING MODEL TO: " + MODEL);
          t.printStackTrace();
        }
        return classifier;
      }
    } else {
      log("no file to save the classifier to.");
      return (E) EntailmentClassifier.loadNoErrors(MODEL);
    }
  }


  /**
   * A helper function to train a classifier.
   * This is useful to factor out for cross-validation.
   *
   * @param data The dataset to train the classifier on.
   * @return A trained classifier.
   */
  public Classifier<Trilean, String> trainClassifier(GeneralDataset<Trilean, String> data) {
    // (create factory)
    forceTrack("Training the classifier");
    startTrack("creating factory");

//    ShiftParamsLogisticClassifierFactory<Trilean, String> factory = new ShiftParamsLogisticClassifierFactory<>();

    LinearClassifierFactory<Trilean, String> factory = new LinearClassifierFactory<>();
    switch (TRAIN_REGULARIZER) {
      case L1:
        factory.setMinimizerCreator(() -> new OWLQNMinimizer(TRAIN_SIGMA).shutUp());
        break;
      case L2:
        factory.setSigma(TRAIN_SIGMA);
        break;
    }
    log("regularizer: " + TRAIN_REGULARIZER);
    log("sigma: " + TRAIN_SIGMA);
    if (TRAIN_FEATURE_COUNT_THRESHOLD > 0) {
      data.applyFeatureCountThreshold(TRAIN_FEATURE_COUNT_THRESHOLD);
      log("feature threshold: " + TRAIN_FEATURE_COUNT_THRESHOLD);
    }
    endTrack("creating factory");
    // (train classifier)
    log("training classifier:");
    Classifier<Trilean, String> classifier = factory.trainClassifier(data);
    // (evaluate training accuracy)
    log("Training accuracy: " + classifier.evaluateAccuracy(data));
    endTrack("Training the classifier");
    return classifier;
  }


  public Classifier<Trilean, String> trainClassifier(Collection<DistantEntailmentPair> data) {
    return trainClassifier(featurizer.featurize(data.stream().map(DistantEntailmentPair::asEntailmentPair)));
  }



  public static void main(String[] args) throws IOException, ClassNotFoundException {
    // Set up some useful objects
    EntailmentFeaturizer featurizer = new EntailmentFeaturizer(args);
    ClassifierTrainer trainer = new ClassifierTrainer(featurizer);
    Execution.fillOptions(trainer, args);
    RedwoodConfiguration.apply(StringUtils.argsToProperties(args));

    // Initialize the cache files
    if (trainer.TRAIN_CACHE == null && trainer.TRAIN_CACHE_DO) {
      trainer.TRAIN_CACHE = new File(trainer.TRAIN_FILE.getPath() + (trainer.TRAIN_COUNT < 0 ? "" : ("." + trainer.TRAIN_COUNT)) + "." + trainer.CLASSIFIER + ".cache");
    }
    if (trainer.TEST_CACHE == null && trainer.TEST_CACHE_DO) {
      trainer.TEST_CACHE = new File(trainer.TEST_FILE.getPath() + "." + trainer.CLASSIFIER + ".cache");
    }

    // Train a classifier
    switch (trainer.CLASSIFIER) {
      case SIMPLE:
        SimpleEntailmentClassifier.train(trainer);
        break;
      case DISTANT:
        DistantEntailmentClassifier.train(trainer);
        break;
      default:
        throw new IllegalStateException("Classifier is not implemented: " + trainer.CLASSIFIER);
    }
  }

}



