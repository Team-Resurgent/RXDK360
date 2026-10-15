namespace RXDK360.SampleBrowser.Models;

public enum Difficulty
{
    Unknown = 0,
    Beginner = 1,
    Intermediate = 2,
    Advanced = 3,
}

public static class DifficultyMap
{
    // Matches the stock SampleBrowser samples.xslt thresholds:
    //   complexity > 1999 -> advanced, > 999 -> intermediate, > 0 -> beginner.
    public static Difficulty FromComplexity(int complexity) => complexity switch
    {
        > 1999 => Difficulty.Advanced,
        > 999 => Difficulty.Intermediate,
        > 0 => Difficulty.Beginner,
        _ => Difficulty.Unknown,
    };
}
